/* SPDX-License-Identifier: MIT */
#include "kk_async_compute.h"
#include "kk_device.h"
#include "kk_shader.h"
#include "kk_perf.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "vk_common_entrypoints.h"
#include "vk_pipeline.h"
#include "util/os_time.h"
#include "kk_compiler_cache.h"
#include "util/disk_cache_os.h"
#include "kk_physical_device.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"
#include <pthread.h>

_Thread_local bool kk_defer_compute;
_Thread_local bool kk_compile_worker;

/* Deferred builds an application binds are remembered per application in the
 * Metal archive's disk namespace, by a digest of their source: the compiler
 * input of a lazily translated shader, otherwise its MSL. In a later run
 * those builds are predicted: they start at once on the predicted-build queue
 * and are usually ready before their first bind, instead of compiling on the
 * binding thread. A device thread loads the set and stores new first binds;
 * the bind path only claims a slot and copies the digest. */
#define KK_BOUND_LIMIT 4096
#define KK_BOUND_STORE_NS 2000000000ull
struct kk_bound_id { uint8_t bytes[16]; };
struct kk_predicted { unsigned count; struct kk_bound_id ids[]; };

struct kk_async_compute_owner {
   pthread_mutex_t mutex;
   pthread_cond_t drained;
   unsigned active;
   struct kk_async_compute *retired, *jobs;
   bool stopping;
   struct kk_device *dev;
   pthread_t bound_thread;
   pthread_cond_t bound_wake;
   bool bound_thread_started, bound_stop;
   struct kk_predicted *predicted; /* published once by the bound-set thread */
   unsigned bound_count;           /* first-bind slots claimed */
   struct { struct kk_bound_id id; unsigned ready; } bound[KK_BOUND_LIMIT];
};

struct kk_async_compute {
   pthread_mutex_t mutex;
   struct kk_async_compute *next, *active_next;
   unsigned refs;
   bool ready, reported, cancel_requested, cancel_sent;
   VkResult result;
   struct kk_device *dev;
   struct kk_shader *shader;
   struct kk_async_compute_owner *owner;
   VkResult (*compile)(struct kk_device *, struct kk_shader *);
   void *task;
   struct kk_perf_compile_scope scope;
   uint64_t request_id, parent_id, start, build_id;
   bool bound, used, cache_miss_reported;
   /* Owner mutex: retired is set by the operation's cleanup; destroy, with a
    * copy of the allocation callbacks, is set when the last shader reference
    * goes away first. */
   bool retired, has_allocator;
   bool has_id, predicted;
   struct kk_bound_id id;
   void (*destroy)(struct vk_device *, struct kk_shader *, const VkAllocationCallbacks *);
   VkAllocationCallbacks allocator;
};

static void
job_unref(struct kk_async_compute *job)
{
   if (!p_atomic_dec_zero(&job->refs)) return;
   mtl_release(job->task);
   pthread_mutex_destroy(&job->mutex);
   free(job);
}

static void
job_result(struct kk_async_compute *job, VkResult result)
{
   pthread_mutex_lock(&job->mutex);
   if (p_atomic_read(&job->cancel_requested)) {
      /* Metal has no safe cancellation API in Wine. Finish the synchronous
       * native call, then discard its result instead of publishing or caching. */
      mtl_release(job->shader->pipeline.cs);
      job->shader->pipeline.cs = NULL;
      result = VK_ERROR_DEVICE_LOST;
   }
   job->result = result;
   /* Release publication covers the native handle and all function metadata. */
   p_atomic_set(&job->ready, true);
   pthread_mutex_unlock(&job->mutex);
   if (job->dev->perf) {
      job->scope.cancelled = p_atomic_read(&job->cancel_requested);
      kk_perf_compile_end(&job->scope, false, result);
      kk_perf_compiler(job->dev->perf, -1);
      kk_perf_async_compute(job->dev->perf, 0, -1);
      kk_perf_add(job->dev->perf, KK_PERF_SHADER_COMPILE, kk_perf_now() - job->start);
   }
}

static void
job_execute(void *data)
{
   struct kk_async_compute *job = data;
   if (p_atomic_read(&job->cancel_requested)) {
      pthread_mutex_lock(&job->mutex);
      job->result = VK_ERROR_DEVICE_LOST;
      p_atomic_set(&job->ready, true);
      pthread_mutex_unlock(&job->mutex);
      if (job->dev->perf) kk_perf_async_compute(job->dev->perf, -1, 0);
      return;
   }
   if (job->dev->perf) {
      job->start = kk_perf_now();
      kk_perf_async_compute(job->dev->perf, -1, 1);
      kk_perf_compiler(job->dev->perf, 1);
      kk_perf_compile_begin(job->dev->perf, &job->scope);
      job->scope.asynchronous = true;
      job->scope.build_id = job->build_id;
      job->scope.request_id = job->request_id;
      job->scope.parent_id = job->parent_id;
      job->scope.input_count = 1;
      job->scope.stage_mask = BITFIELD_BIT(MESA_SHADER_COMPUTE);
   }
   /* Also when the join runs the build on the API thread: the join reports
    * the result. */
   kk_compile_worker = true;
   VkResult result = job->compile(job->dev, job->shader);
   kk_compile_worker = false;
   job_result(job, result);
}

static void
job_failed(void *data, const char *reason)
{
   struct kk_async_compute *job = data;
   fprintf(stderr, "KosmicKrisp: asynchronous compute exception: %s\n",
           reason ? reason : "unknown native exception");
   /* Once native readiness is published, a best-effort cache exception must
    * neither invalidate that PSO nor finish its tracking scope twice. */
   if (!p_atomic_read(&job->ready))
      job_result(job, VK_ERROR_INVALID_SHADER_NV);
}

/* The operation is finished, but Vulkan allocation/debug callbacks must stay
 * on API caller threads. Transfer its reference to the device retirement list. */
static void
job_cleanup(void *data)
{
   struct kk_async_compute *job = data;
   struct kk_async_compute_owner *owner = job->owner;
   if (!p_atomic_read(&job->ready)) {
      /* A queued NSOperation cancelled before execution still runs cleanup. */
      pthread_mutex_lock(&job->mutex);
      job->result = VK_ERROR_DEVICE_LOST;
      p_atomic_set(&job->ready, true);
      pthread_mutex_unlock(&job->mutex);
      if (job->dev->perf) kk_perf_async_compute(job->dev->perf, -1, 0);
   }
   pthread_mutex_lock(&owner->mutex);
   struct kk_async_compute **link = &owner->jobs;
   while (*link != job) { assert(*link); link = &(*link)->active_next; }
   *link = job->active_next;
   job->retired = true;
   job->next = owner->retired;
   p_atomic_set(&owner->retired, job);
   --owner->active;
   pthread_cond_broadcast(&owner->drained);
   pthread_mutex_unlock(&owner->mutex);
}

static void
job_final_event(struct kk_async_compute *job)
{
   if (job->build_id)
      kk_perf_build_event(job->dev->perf, job->build_id,
         p_atomic_read(&job->cancel_requested) ? "cancelled" :
         job->result != VK_SUCCESS ? "failed" :
         p_atomic_read(&job->used) ? "completed-used" : "completed-unused", NULL, 0);
}

/* Runs on API threads. A shader whose last reference went away while its
 * build was still queued or compiling is destroyed here, after the build
 * retired, with the allocation callbacks copied at destruction. */
void
kk_async_compute_drain(struct kk_device *dev)
{
   struct kk_async_compute_owner *owner = p_atomic_read(&dev->async_compute);
   if (!owner || !p_atomic_read(&owner->retired)) return;
   pthread_mutex_lock(&owner->mutex);
   struct kk_async_compute *job = owner->retired;
   p_atomic_set(&owner->retired, NULL);
   pthread_mutex_unlock(&owner->mutex);
   while (job) {
      struct kk_async_compute *next = job->next;
      if (job->destroy) {
         struct kk_shader *shader = job->shader;
         job_final_event(job);
         shader->pipeline.compute.pending = NULL;
         job->destroy(&job->dev->vk, shader, job->has_allocator ? &job->allocator : NULL);
         job_unref(job); /* the shader's reference */
      }
      job_unref(job);
      job = next;
   }
}

static int
bound_id_compare(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct kk_bound_id));
}

/* The source of a build: a lazily translated shader's recipe, otherwise its
 * MSL. Both are immutable while the build exists. */
static const void *
job_source(const struct kk_async_compute *job, size_t *size)
{
   const struct kk_lazy_compute *lazy = job->shader->lazy;
   if (lazy) {
      *size = lazy->recipe_size;
      return lazy->recipe;
   }
   const char *code = job->shader->msl_data[MESA_SHADER_COMPUTE].code;
   *size = code ? strlen(code) : 0;
   return code;
}

static void
job_identify(struct kk_async_compute *job)
{
   if (job->has_id) return;
   size_t size;
   const void *source = job_source(job, &size);
   if (!source) return;
   XXH128_hash_t hash = XXH3_128bits(source, size);
   memcpy(job->id.bytes, &hash.low64, 8);
   memcpy(job->id.bytes + 8, &hash.high64, 8);
   job->has_id = true;
}

/* First bind of this build, once per build: no lock or wait. */
static void
job_record_bind(struct kk_async_compute *job)
{
   struct kk_async_compute_owner *owner = job->owner;
   job_identify(job);
   if (!job->has_id) return;
   unsigned slot = p_atomic_inc_return(&owner->bound_count) - 1;
   if (slot >= KK_BOUND_LIMIT) return;
   owner->bound[slot].id = job->id;
   p_atomic_set(&owner->bound[slot].ready, 1);
}

#ifdef ENABLE_SHADER_CACHE
/* Every device of the application stores into one journal, and a load takes
 * its newest record. A store therefore merges the newest stored set and is
 * written past its sequence: a long-lived device storing after short-lived
 * devices (one per test case, say) advanced the journal must not publish an
 * older record, and a short-lived device must not publish a newer record
 * without the long-lived device's binds. Stores run on bound-set threads
 * only; within the process they are serialized. */
static pthread_mutex_t bound_store_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Stores the newest stored set, the loaded set and every first bind recorded
 * so far; returns how many recorded slots the stored set covers. */
static unsigned
bound_store(struct kk_async_compute_owner *owner, struct disk_cache *disk,
            const cache_key parent, uint64_t *sequence, unsigned stored)
{
   unsigned count = MIN2(p_atomic_read(&owner->bound_count), KK_BOUND_LIMIT);
   unsigned ready = stored;
   while (ready < count && p_atomic_read(&owner->bound[ready].ready)) ++ready;
   if (ready == stored) return stored;
   pthread_mutex_lock(&bound_store_mutex);
   uint64_t newest = 0;
   size_t size = 0;
   void *current = kk_compiler_index_load(disk, parent, &newest, &size);
   unsigned current_count = size % sizeof(struct kk_bound_id) ? 0 :
      MIN2(size / sizeof(struct kk_bound_id), KK_BOUND_LIMIT);
   const struct kk_predicted *loaded = owner->predicted;
   unsigned previous = loaded ? loaded->count : 0;
   unsigned total = ready + previous + current_count, unique = 0;
   struct kk_bound_id *ids = malloc(total * sizeof(*ids));
   bool complete = false;
   if (ids) {
      for (unsigned i = 0; i < ready; ++i) ids[i] = owner->bound[i].id;
      if (previous) memcpy(ids + ready, loaded->ids, previous * sizeof(*ids));
      if (current_count) memcpy(ids + ready + previous, current, current_count * sizeof(*ids));
      qsort(ids, total, sizeof(*ids), bound_id_compare);
      for (unsigned i = 0; i < total; ++i)
         if (!unique || bound_id_compare(&ids[unique - 1], &ids[i])) ids[unique++] = ids[i];
      unique = MIN2(unique, KK_BOUND_LIMIT);
      *sequence = MAX2(*sequence, newest);
      complete = kk_compiler_index_store(disk, parent, sequence, ids, unique * sizeof(*ids));
      free(ids);
   }
   free(current);
   pthread_mutex_unlock(&bound_store_mutex);
   /* An incomplete store is retried at the next period. */
   return complete ? ready : stored;
}

/* Builds submitted before the set was published start on a predicted-build
 * worker now. A build submitted after publication read the set itself. */
static void
bound_promote(struct kk_async_compute_owner *owner, const struct kk_predicted *loaded)
{
   pthread_mutex_lock(&owner->mutex);
   unsigned capacity = owner->active, count = 0;
   struct kk_async_compute **jobs = capacity ? malloc(capacity * sizeof(*jobs)) : NULL;
   for (struct kk_async_compute *job = owner->jobs; job && jobs && count < capacity;
        job = job->active_next) {
      if (!p_atomic_read(&job->task) || p_atomic_read(&job->predicted) ||
          p_atomic_read(&job->ready) || !job->has_id ||
          !bsearch(&job->id, loaded->ids, loaded->count, sizeof(loaded->ids[0]), bound_id_compare))
         continue;
      p_atomic_set(&job->predicted, true);
      p_atomic_inc(&job->refs);
      jobs[count++] = job;
   }
   pthread_mutex_unlock(&owner->mutex);
   for (unsigned i = 0; i < count; ++i) {
      struct kk_async_compute *job = jobs[i];
      if (job->build_id)
         kk_perf_build_event(job->dev->perf, job->build_id, "predicted", NULL, 0);
      mtl_compiler_task_prestart(job->dev->mtl_compiler_handle, job->task);
      job_unref(job);
   }
   free(jobs);
}

static void *
bound_thread_main(void *data)
{
   struct kk_async_compute_owner *owner = data;
   struct kk_physical_device *pdev = kk_device_physical(owner->dev);
   pthread_setname_np("KosmicKrisp bound builds");
   struct disk_cache *disk = kk_open_disk_cache(pdev->archive_cache_digest,
      pdev->info.gpu_apple_family, pdev->settings.disabled_workarounds);
   cache_key parent;
   uint64_t sequence = 0;
   if (disk) {
      /* KK_DEBUG_MSL_SALT starts a cold Metal cache; it also starts an empty set. */
      struct { char tag[sizeof("KosmicKrisp compute first binds 1")]; blake3_hash salt; } input = {
         .tag = "KosmicKrisp compute first binds 1" };
      const char *salt = getenv("KK_DEBUG_MSL_SALT");
      if (salt && *salt) _mesa_blake3_compute(salt, strlen(salt), input.salt);
      disk_cache_compute_key(disk, &input, sizeof(input), parent);
      size_t size = 0;
      void *blob = kk_compiler_index_load(disk, parent, &sequence, &size);
      unsigned count = size / sizeof(struct kk_bound_id);
      struct kk_predicted *loaded = blob && count && count <= KK_BOUND_LIMIT &&
         size == count * sizeof(struct kk_bound_id) ?
         malloc(sizeof(*loaded) + size) : NULL;
      if (loaded) {
         loaded->count = count;
         memcpy(loaded->ids, blob, size);
         qsort(loaded->ids, count, sizeof(loaded->ids[0]), bound_id_compare);
         p_atomic_set(&owner->predicted, loaded);
         bound_promote(owner, loaded);
      }
      free(blob);
   }
   unsigned stored = 0;
   pthread_mutex_lock(&owner->mutex);
   for (;;) {
      if (!owner->bound_stop) {
         struct timespec wait = { KK_BOUND_STORE_NS / 1000000000ull, KK_BOUND_STORE_NS % 1000000000ull };
         pthread_cond_timedwait_relative_np(&owner->bound_wake, &owner->mutex, &wait);
      }
      bool stop = owner->bound_stop;
      pthread_mutex_unlock(&owner->mutex);
      if (disk) stored = bound_store(owner, disk, parent, &sequence, stored);
      if (stop) break;
      pthread_mutex_lock(&owner->mutex);
   }
   if (disk) disk_cache_destroy(disk);
   return NULL;
}
#endif

/* The device's build owner, created once; its bound-set thread starts with it. */
static struct kk_async_compute_owner *
owner_get(struct kk_device *dev)
{
   struct kk_async_compute_owner *owner = p_atomic_read(&dev->async_compute);
   if (!owner) {
      simple_mtx_lock(&dev->pipeline_cache_init_mutex);
      owner = dev->async_compute;
      if (!owner) {
         owner = calloc(1, sizeof(*owner));
         if (owner && pthread_mutex_init(&owner->mutex, NULL)) {
            free(owner); owner = NULL;
         } else if (owner && pthread_cond_init(&owner->drained, NULL)) {
            pthread_mutex_destroy(&owner->mutex); free(owner); owner = NULL;
         } else if (owner && pthread_cond_init(&owner->bound_wake, NULL)) {
            pthread_cond_destroy(&owner->drained);
            pthread_mutex_destroy(&owner->mutex); free(owner); owner = NULL;
         }
         if (owner) {
            owner->dev = dev;
            /* Prediction is best effort; without the thread nothing is predicted. */
#ifdef ENABLE_SHADER_CACHE
            owner->bound_thread_started = disk_cache_enabled() &&
               !pthread_create(&owner->bound_thread, NULL, bound_thread_main, owner);
#endif
            p_atomic_set(&dev->async_compute, owner);
         }
      }
      simple_mtx_unlock(&dev->pipeline_cache_init_mutex);
   }
   return owner;
}

/* At device creation, so the bound set is usually published before the first
 * pipeline; builds submitted before that are promoted when it publishes. */
void
kk_async_compute_device_init(struct kk_device *dev)
{
   const char *eager = getenv("KK_DEBUG_EAGER_COMPUTE_TRANSLATION");
   dev->eager_compute_translation = eager && *eager && strcmp(eager, "0");
   const char *fail = getenv("KK_DEBUG_FAIL_COMPUTE_TRANSLATION");
   dev->fail_compute_translation = fail ? strtoul(fail, NULL, 10) : 0;
   owner_get(dev);
}

bool
kk_async_compute_start(struct kk_device *dev, struct kk_shader *shader,
                        VkResult (*compile)(struct kk_device *, struct kk_shader *))
{
   struct kk_async_compute_owner *owner = owner_get(dev);
   if (!owner) return false;
   struct kk_async_compute *job = calloc(1, sizeof(*job));
   if (!job) return false;
   if (pthread_mutex_init(&job->mutex, NULL)) { free(job); return false; }
   job->refs = 2; /* shader and queued operation cleanup */
   job->result = VK_NOT_READY;
   job->dev = dev; job->shader = shader; job->owner = owner; job->compile = compile;
   /* Identified before it is visible to the bound-set thread's promotion. */
   job_identify(job);
   if (dev->perf) {
      kk_perf_compile_parent(dev->perf, &job->request_id, &job->parent_id);
      size_t size;
      const void *source = job_source(job, &size);
      job->build_id = source ? kk_perf_build_queued(dev->perf, source, size) : 0;
   }
   pthread_mutex_lock(&owner->mutex);
   assert(!owner->stopping);
   ++owner->active;
   job->active_next = owner->jobs;
   owner->jobs = job;
   pthread_mutex_unlock(&owner->mutex);
   shader->pipeline.compute.pending = job;
   /* Read after linking: a set published later promotes this build once its
    * task is visible. */
   const struct kk_predicted *predicted = p_atomic_read(&owner->predicted);
   bool is_predicted = predicted && job->has_id && bsearch(&job->id, predicted->ids,
      predicted->count, sizeof(predicted->ids[0]), bound_id_compare);
   if (is_predicted) {
      p_atomic_set(&job->predicted, true);
      if (job->build_id)
         kk_perf_build_event(dev->perf, job->build_id, "predicted", NULL, 0);
   }
   if (dev->perf) kk_perf_async_compute(dev->perf, 1, 0);
   p_atomic_set(&job->task, (is_predicted ? mtl_compiler_submit_predicted : mtl_compiler_submit)(
      dev->mtl_compiler_handle, job, job_execute, job_failed, job_cleanup));
   if (job->task) return true;
   /* Queue allocation is best effort; ordinary synchronous compilation remains
    * available with no published pending shader on this failure path. */
   shader->pipeline.compute.pending = NULL;
   if (job->build_id) kk_perf_build_event(dev->perf, job->build_id, "synchronous-fallback", NULL, 0);
   if (dev->perf) kk_perf_async_compute(dev->perf, -1, 0);
   pthread_mutex_lock(&owner->mutex);
   --owner->active;
   struct kk_async_compute **link = &owner->jobs;
   while (*link != job) { assert(*link); link = &(*link)->active_next; }
   *link = job->active_next;
   pthread_cond_broadcast(&owner->drained);
   pthread_mutex_unlock(&owner->mutex);
   job_unref(job);
   job_unref(job);
   return false;
}

static void
job_wait(struct kk_async_compute *job, const char *reason)
{
   if (p_atomic_read(&job->ready)) return;
   static const char *const modes[] = {
      [MTL_COMPILER_JOIN_NONE] = "none", [MTL_COMPILER_JOIN_HERE] = "here",
      [MTL_COMPILER_JOIN_QUEUED] = "queued", [MTL_COMPILER_JOIN_RUNNING] = "running",
   };
   uint64_t start = job->dev->perf ? kk_perf_now() : 0;
   enum mtl_compiler_join join = mtl_compiler_task_join(job->task);
   if (start)
      kk_perf_async_join(job->dev->perf, job->parent_id, reason, modes[join],
                          start, kk_perf_now() - start);
}

static VkResult
join_shader(struct kk_shader *shader, const char *reason)
{
   if (!shader || shader->info.stage != MESA_SHADER_COMPUTE) return VK_SUCCESS;
   struct kk_async_compute *job = shader->pipeline.compute.pending;
   if (!job) return VK_SUCCESS;
   job_wait(job, reason);
   if (p_atomic_read(&job->cancel_requested)) return VK_ERROR_DEVICE_LOST;
   if (job->result == VK_SUCCESS) return VK_SUCCESS;
   /* A failed build fails each use that needs it, with out of host memory as
    * other Mesa drivers report a failed internal build; the device stays
    * usable for everything else. */
   if (!p_atomic_cmpxchg(&job->reported, false, true))
      mesa_loge("compute pipeline %p failed native compilation (%d); work that binds it fails",
                (void *)shader, job->result);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult
kk_async_compute_join(struct kk_shader *shader)
{
   struct kk_async_compute *job = shader ? shader->pipeline.compute.pending : NULL;
   if (job && !p_atomic_read(&job->bound) && !p_atomic_cmpxchg(&job->bound, false, true)) {
      if (job->build_id)
         kk_perf_build_event(job->dev->perf, job->build_id, "first-bind", NULL, 0);
      job_record_bind(job);
   }
   return join_shader(shader, "bind");
}

void
kk_async_compute_dispatch(struct kk_shader *shader)
{
   struct kk_async_compute *job = shader ? shader->pipeline.compute.pending : NULL;
   if (job && job->build_id && !p_atomic_cmpxchg(&job->used, false, true))
      kk_perf_build_event(job->dev->perf, job->build_id, "first-dispatch", NULL, 0);
}

/* The serialized shader is its MSL and metadata, never the native pipeline,
 * and a restored shader defers its native build again. A pending build is
 * therefore cached at creation, so a later launch skips NIR and MSL for it
 * even if this process never builds it; only a failed build is not cached. */
bool
kk_async_compute_serializable(struct kk_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_COMPUTE) return true;
   struct kk_async_compute *job = shader->pipeline.compute.pending;
   if (!job) return true;
   pthread_mutex_lock(&job->mutex);
   bool ready = job->ready;
   bool report = !ready && job->build_id && !job->cache_miss_reported;
   if (report) job->cache_miss_reported = true;
   bool cacheable = !ready || job->result == VK_SUCCESS;
   pthread_mutex_unlock(&job->mutex);
   if (report) {
      struct vk_pipeline_cache_object *object = &shader->vk.pipeline.cache_obj;
      kk_perf_build_event(job->dev->perf, job->build_id, "cache-key-miss",
         object->key_data, object->key_data ? object->key_size : 0);
   }
   return cacheable;
}

/* Called when the last reference to the shader goes away, so no pipeline,
 * cache or bind can use its native build again. A build that has not entered
 * its native compile is cancelled without compiling; a build already
 * compiling finishes and its result is discarded. Destruction never waits for
 * either: while the compile queue still owns the build, the shader and a copy
 * of the allocation callbacks move to it and the first drain after the build
 * retires destroys the shader. Returns false when the build has retired, and
 * the caller destroys the shader now. */
bool
kk_async_compute_defer_destroy(struct kk_shader *shader, const VkAllocationCallbacks *allocator,
   void (*destroy)(struct vk_device *, struct kk_shader *, const VkAllocationCallbacks *))
{
   struct kk_async_compute *job = shader->pipeline.compute.pending;
   if (!p_atomic_read(&job->ready)) {
      p_atomic_set(&job->cancel_requested, true);
      mtl_compiler_task_cancel(job->task);
   }
   struct kk_async_compute_owner *owner = job->owner;
   pthread_mutex_lock(&owner->mutex);
   bool deferred = !job->retired;
   if (deferred) {
      if (allocator) job->allocator = *allocator;
      job->has_allocator = allocator != NULL;
      job->destroy = destroy;
   }
   pthread_mutex_unlock(&owner->mutex);
   /* Once deferred, a drain on another thread may free the job and shader. */
   return deferred;
}

/* The build has retired: its operation finished and its cleanup ran. */
void
kk_async_compute_shader_finish(struct kk_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_COMPUTE) return;
   struct kk_async_compute *job = shader->pipeline.compute.pending;
   if (!job) return;
   job_final_event(job);
   shader->pipeline.compute.pending = NULL;
   job_unref(job);
}

void
kk_async_compute_device_finish(struct kk_device *dev)
{
   struct kk_async_compute_owner *owner = dev->async_compute;
   if (!owner) return;
   pthread_mutex_lock(&owner->mutex);
   owner->stopping = true;
   for (struct kk_async_compute *job = owner->jobs; job; job = job->active_next)
      if (!p_atomic_read(&job->ready)) p_atomic_set(&job->cancel_requested, true);
   pthread_mutex_unlock(&owner->mutex);
   for (;;) {
      pthread_mutex_lock(&owner->mutex);
      struct kk_async_compute *job = owner->jobs;
      while (job && (job->cancel_sent || p_atomic_read(&job->ready))) job = job->active_next;
      if (job) {
         job->cancel_sent = true;
         p_atomic_inc(&job->refs);
      }
      pthread_mutex_unlock(&owner->mutex);
      if (!job) break;
      /* No owner lock across cancel: Foundation may publish completion now. */
      mtl_compiler_task_cancel(job->task);
      job_unref(job);
   }
   /* Builds still active are compiling at background priority. Raise their
    * workers so device destruction does not wait at that priority. */
   pthread_mutex_lock(&owner->mutex);
   unsigned raised_count = 0, capacity = owner->active;
   void **raised = capacity ? calloc(capacity, sizeof(*raised)) : NULL;
   for (struct kk_async_compute *job = owner->jobs; job && raised && raised_count < capacity;
        job = job->active_next) {
      void *token = mtl_compiler_task_raise(job->task);
      if (token) raised[raised_count++] = token;
   }
   while (owner->active) pthread_cond_wait(&owner->drained, &owner->mutex);
   pthread_mutex_unlock(&owner->mutex);
   for (unsigned i = 0; i < raised_count; ++i) mtl_compiler_task_raise_end(raised[i]);
   free(raised);
   kk_async_compute_drain(dev);
   if (owner->bound_thread_started) {
      pthread_mutex_lock(&owner->mutex);
      owner->bound_stop = true;
      pthread_cond_broadcast(&owner->bound_wake);
      pthread_mutex_unlock(&owner->mutex);
      pthread_join(owner->bound_thread, NULL);
   }
   free(owner->predicted);
   pthread_cond_destroy(&owner->bound_wake);
   pthread_cond_destroy(&owner->drained);
   pthread_mutex_destroy(&owner->mutex);
   free(owner);
   dev->async_compute = NULL;
}

bool
kk_async_compute_can_defer(uint32_t count, const VkComputePipelineCreateInfo *infos)
{
   if (count != 1 || vk_validate_shader_binaries()) return false;
   /* A cache-only creation (FAIL_ON_PIPELINE_COMPILE_REQUIRED) never compiles
    * on a miss; a shader it restores from the cache defers its build like any
    * other restore, and kk_async_compute_finish_creation does not wait for it. */
   const VkPipelineCreateFlags2KHR synchronous =
      VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR |
      VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR |
      VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
      VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
   return !(vk_compute_pipeline_create_flags(infos) & synchronous) &&
      !vk_find_struct_const(infos->pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
}

VkResult
kk_async_compute_finish_creation(struct kk_device *dev, uint32_t count,
   const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *allocator,
   VkPipeline *pipelines, VkResult result, bool force_wait)
{
   for (uint32_t i = 0; i < count; ++i) {
      if (pipelines[i] == VK_NULL_HANDLE) continue;
      struct vk_pipeline *pipeline = vk_pipeline_from_handle(pipelines[i]);
      struct vk_shader *vk_shader = pipeline->ops->get_shader(pipeline, MESA_SHADER_COMPUTE);
      if (!vk_shader) continue;
      struct kk_shader *shader = container_of(vk_shader, struct kk_shader, vk);
      struct kk_async_compute *job = shader->pipeline.compute.pending;
      if (!job) continue;
      bool ready = p_atomic_read(&job->ready);
      const VkPipelineCreationFeedbackCreateInfo *feedback =
         vk_find_struct_const(infos[i].pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
      if (!ready && (vk_compute_pipeline_create_flags(&infos[i]) &
             VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)) {
         /* Common cache-only creation can return a shader only on a hit.
          * Reuse its existing pending job: this request schedules no compile
          * and must not wait for it. The ordinary bind joins the same job. */
         continue;
      }
      VkResult status = VK_SUCCESS;
      if (force_wait || (ready && job->result != VK_SUCCESS)) {
         uint64_t begin = feedback ? os_time_get_nano() : 0;
         status = join_shader(shader, "creation");
         if (feedback) {
            uint64_t duration = os_time_get_nano() - begin;
            feedback->pPipelineCreationFeedback->duration += duration;
            if (!ready) feedback->pPipelineCreationFeedback->flags &=
               ~VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
            if (feedback->pipelineStageCreationFeedbackCount) {
               feedback->pPipelineStageCreationFeedbacks[0].duration += duration;
               if (!ready) feedback->pPipelineStageCreationFeedbacks[0].flags &=
                  ~VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
            }
         }
      }
      if (!ready && feedback) {
         feedback->pPipelineCreationFeedback->flags &=
            ~VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
         if (feedback->pipelineStageCreationFeedbackCount)
            feedback->pPipelineStageCreationFeedbacks[0].flags &=
               ~VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }
      if (status != VK_SUCCESS) {
         vk_common_DestroyPipeline(vk_device_to_handle(&dev->vk), pipelines[i], allocator);
         pipelines[i] = VK_NULL_HANDLE;
         if (result == VK_SUCCESS) result = status;
      }
   }
   return result;
}
