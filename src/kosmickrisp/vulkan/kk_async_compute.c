/* SPDX-License-Identifier: MIT */
#include "kk_async_compute.h"
#include "kk_device.h"
#include "kk_shader.h"
#include "kk_perf.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "vk_common_entrypoints.h"
#include "vk_pipeline.h"
#include "util/blob.h"
#include "util/os_time.h"
#include <pthread.h>

_Thread_local bool kk_defer_compute;

struct kk_async_compute_owner {
   pthread_mutex_t mutex;
   pthread_cond_t drained;
   unsigned active;
   struct kk_async_compute *retired, *jobs;
   bool stopping;
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
   /* Borrowed from shader: destruction joins task retirement before freeing
    * the shader or its embedded cache object. An owning reference here would
    * prevent that destruction from cancelling an otherwise unneeded job. */
   struct vk_pipeline_cache_object *cache_object;
   struct blob cache_blob;
   struct kk_perf_compile_scope scope;
   uint64_t request_id, parent_id, start, build_id;
   bool bound, used;
};

static void
job_unref(struct kk_async_compute *job)
{
   if (!p_atomic_dec_zero(&job->refs)) return;
   blob_finish(&job->cache_blob);
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
   struct vk_pipeline_cache_object *object = job->cache_object;
   job->cache_object = NULL;
   pthread_mutex_unlock(&job->mutex);
   if (job->dev->perf) {
      job->scope.cancelled = p_atomic_read(&job->cancel_requested);
      kk_perf_compile_end(&job->scope, false, result);
      kk_perf_compiler(job->dev->perf, -1);
      kk_perf_async_compute(job->dev->perf, 0, -1);
      kk_perf_add(job->dev->perf, KK_PERF_SHADER_COMPILE, kk_perf_now() - job->start);
   }
   if (result == VK_SUCCESS && object) {
      struct blob *blob = &job->cache_blob;
      blob_init(blob);
      /* The authoritative common object codec invokes our now-ready shader
       * serializer. It never sees a caller cache pointer or half-made PSO. */
      if (object->ops->serialize(object, blob) && !blob->out_of_memory)
         mtl_compiler_store_shader(job->dev->mtl_compiler_handle,
            object->key_data, object->key_size, blob->data, blob->size);
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
   job_result(job, job->compile(job->dev, job->shader));
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
   job->next = owner->retired;
   p_atomic_set(&owner->retired, job);
   --owner->active;
   pthread_cond_broadcast(&owner->drained);
   pthread_mutex_unlock(&owner->mutex);
}

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
      job_unref(job);
      job = next;
   }
}

bool
kk_async_compute_start(struct kk_device *dev, struct kk_shader *shader,
                        VkResult (*compile)(struct kk_device *, struct kk_shader *))
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
         }
         if (owner) p_atomic_set(&dev->async_compute, owner);
      }
      simple_mtx_unlock(&dev->pipeline_cache_init_mutex);
   }
   if (!owner) return false;
   struct kk_async_compute *job = calloc(1, sizeof(*job));
   if (!job) return false;
   if (pthread_mutex_init(&job->mutex, NULL)) { free(job); return false; }
   job->refs = 2; /* shader and queued operation cleanup */
   job->result = VK_NOT_READY;
   job->dev = dev; job->shader = shader; job->owner = owner; job->compile = compile;
   if (dev->perf) {
      kk_perf_compile_parent(dev->perf, &job->request_id, &job->parent_id);
      job->build_id = kk_perf_build_queued(dev->perf,
         shader->msl_data[MESA_SHADER_COMPUTE].code);
   }
   pthread_mutex_lock(&owner->mutex);
   assert(!owner->stopping);
   ++owner->active;
   job->active_next = owner->jobs;
   owner->jobs = job;
   pthread_mutex_unlock(&owner->mutex);
   shader->pipeline.compute.pending = job;
   if (dev->perf) kk_perf_async_compute(dev->perf, 1, 0);
   job->task = mtl_compiler_submit(dev->mtl_compiler_handle, job,
      job_execute, job_failed, job_cleanup);
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
job_wait(struct kk_async_compute *job, const char *reason, bool require_finished)
{
   bool pending = !p_atomic_read(&job->ready);
   if (!pending && !require_finished) return;
   static const char *const modes[] = {
      [MTL_COMPILER_JOIN_NONE] = "none", [MTL_COMPILER_JOIN_HERE] = "here",
      [MTL_COMPILER_JOIN_QUEUED] = "queued", [MTL_COMPILER_JOIN_RUNNING] = "running",
   };
   uint64_t start = pending && job->dev->perf ? kk_perf_now() : 0;
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
   job_wait(job, reason, false);
   if (p_atomic_read(&job->cancel_requested)) return VK_ERROR_DEVICE_LOST;
   if (job->result == VK_SUCCESS) return VK_SUCCESS;
   if (p_atomic_cmpxchg(&job->reported, false, true))
      return VK_ERROR_DEVICE_LOST;
   fprintf(stderr, "KosmicKrisp: compute pipeline %p failed native compilation (%d); first use cannot continue\n",
              (void *)shader, job->result);
   return vk_device_set_lost(&job->dev->vk, "Asynchronous compute pipeline compilation failed (%d)", job->result);
}

VkResult
kk_async_compute_join(struct kk_shader *shader)
{
   struct kk_async_compute *job = shader ? shader->pipeline.compute.pending : NULL;
   if (job && job->build_id && !p_atomic_cmpxchg(&job->bound, false, true))
      kk_perf_build_event(job->dev->perf, job->build_id, "first-bind", NULL, 0);
   return join_shader(shader, "bind");
}

void
kk_async_compute_dispatch(struct kk_shader *shader)
{
   struct kk_async_compute *job = shader ? shader->pipeline.compute.pending : NULL;
   if (job && job->build_id && !p_atomic_cmpxchg(&job->used, false, true))
      kk_perf_build_event(job->dev->perf, job->build_id, "first-dispatch", NULL, 0);
}

bool
kk_async_compute_serializable(struct kk_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_COMPUTE) return true;
   struct kk_async_compute *job = shader->pipeline.compute.pending;
   if (!job) return true;
   pthread_mutex_lock(&job->mutex);
   bool ready = job->ready;
   if (!ready && !job->cache_object && !job->dev->vk.disable_internal_cache) {
      struct vk_pipeline_cache_object *object = &shader->vk.pipeline.cache_obj;
      if (object->ops && object->key_data) {
         job->cache_object = object;
         if (job->build_id)
            kk_perf_build_event(job->dev->perf, job->build_id, "cache-key-miss",
               object->key_data, object->key_size);
      }
   }
   bool success = ready && job->result == VK_SUCCESS;
   pthread_mutex_unlock(&job->mutex);
   return success;
}

/* Called when the last reference to the shader goes away, so no pipeline,
 * cache or bind can use its native build again. A build that has not entered
 * its native compile is cancelled without compiling. Only a build already
 * compiling is joined, and its result is discarded. */
void
kk_async_compute_shader_finish(struct kk_shader *shader)
{
   if (shader->info.stage != MESA_SHADER_COMPUTE) return;
   struct kk_async_compute *job = shader->pipeline.compute.pending;
   if (!job) return;
   if (!p_atomic_read(&job->ready)) {
      p_atomic_set(&job->cancel_requested, true);
      mtl_compiler_task_cancel(job->task);
   }
   job_wait(job, "destroy", true);
   if (job->build_id)
      kk_perf_build_event(job->dev->perf, job->build_id,
         p_atomic_read(&job->cancel_requested) ? "cancelled" :
         job->result != VK_SUCCESS ? "failed" :
         p_atomic_read(&job->used) ? "completed-used" : "completed-unused", NULL, 0);
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
   pthread_cond_destroy(&owner->drained);
   pthread_mutex_destroy(&owner->mutex);
   free(owner);
   dev->async_compute = NULL;
}

bool
kk_async_compute_can_defer(uint32_t count, const VkComputePipelineCreateInfo *infos)
{
   if (count != 1 || vk_validate_shader_binaries()) return false;
   const VkPipelineCreateFlags2KHR synchronous =
      VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR |
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
