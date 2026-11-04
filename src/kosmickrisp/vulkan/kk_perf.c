/* SPDX-License-Identifier: MIT */
#include "kk_perf.h"
#include "kk_device.h"
#include "kk_cmd_buffer.h"
#include "util/os_time.h"
#include "util/mesa-blake3.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct kk_perf_stat { uint64_t count, total, max; };
struct kk_perf_gpu_stat {
   uint64_t frame, count, commands, total, max;
};
struct kk_perf {
   pthread_mutex_t mutex;
   FILE *log;
   bool close_log, compile_details;
   uint64_t compile_sequence;
   uint64_t created, interval_start, frames, interval_frames;
   uint64_t frame_interval, time_interval, sequence;
   int64_t pending, submit_pending, compiler_active;
   uint64_t pending_peak, submit_peak, compiler_peak, invalid_gpu;
   struct kk_perf_stat stat[KK_PERF_METRIC_COUNT];
   struct kk_perf_gpu_stat gpu[256];
   unsigned gpu_count;
};

static const char *const names[KK_PERF_METRIC_COUNT] = {
   "record", "replay", "encode", "submit", "acquire", "present",
   "present_wait", "fence_wait", "semaphore_wait", "sync_wait",
   "queue_wait", "drawable_wait", "deferred", "pipeline_create",
   "shader_compile", "nir", "msl", "metal_library", "metal_pipeline",
   "cache_hit", "cache_miss", "cache_binary_hit", "metal_library_pair",
   "command_bo_allocate", "command_bo_reuse", "execution_residency_commit",
   "command_begin", "command_end", "command_reset", "command_pool_reset",
   "sampled_view_create", "sampled_view_get"
};

uint64_t kk_perf_now(void) { return os_time_get_nano(); }

static void flush_locked(struct kk_perf *, uint64_t, bool);

static uint64_t
positive_option(const char *name, uint64_t fallback)
{
   const char *text = getenv(name);
   if (!text || !*text) return fallback;
   char *end;
   errno = 0;
   unsigned long long value = strtoull(text, &end, 10);
   return !errno && !*end && value && value <= UINT32_MAX ? value : fallback;
}

struct kk_perf *
kk_perf_create(void)
{
   const char *enabled = getenv("KK_PERF_TRACKING");
   if (!enabled || (!strcmp(enabled, "0")) || !strcmp(enabled, "false"))
      return NULL;
   struct kk_perf *p = calloc(1, sizeof(*p));
   if (!p) return NULL;
   if (pthread_mutex_init(&p->mutex, NULL)) { free(p); return NULL; }
   const char *details = getenv("KK_PERF_TRACKING_COMPILES");
   p->compile_details = details && *details && strcmp(details, "0") && strcmp(details, "false");
   p->log = stderr;
   const char *path = getenv("KK_PERF_TRACKING_LOG");
   if (path && *path) {
      FILE *file = fopen(path, "a");
      if (file) { p->log = file; p->close_log = true; }
      else fprintf(stderr, "KosmicKrisp: cannot open performance log %s: %s\n",
                   path, strerror(errno));
   }
   p->frame_interval = positive_option("KK_PERF_TRACKING_INTERVAL_FRAMES", 60);
   p->time_interval = positive_option("KK_PERF_TRACKING_INTERVAL_MS", 1000) * 1000000;
   p->created = p->interval_start = kk_perf_now();
   /* Acknowledge enabled tracking before any native device-init compile. */
   flush_locked(p, kk_perf_now(), false);
   return p;
}

/* Caller holds the diagnostic mutex. Durations are nested CPU-side elapsed
 * spans, not additive CPU utilization. GPU spans can overlap across commits. */
static void
flush_locked(struct kk_perf *p, uint64_t now, bool final)
{
   struct timespec wall;
   clock_gettime(CLOCK_REALTIME, &wall);
   const char *kind = final ? "final" : p->sequence ? "interval" : "init";
   fprintf(p->log, "{\"kk_perf\":1,\"kind\":\"%s\",\"wall_unix_ms\":%.3f,\"device\":\"%p\",\"interval\":%"PRIu64
           ",\"elapsed_ms\":%.6f,\"window_ms\":%.6f,\"frames\":%"PRIu64
           ",\"frame_end\":%"PRIu64",\"final\":%s,\"cpu_spans\":\"nested_wall_ms\","
           "\"gpu_scope\":\"Metal4_commit_feedback_by_CPU_present_boundary\",\"metrics\":{",
           kind, (double)wall.tv_sec * 1000.0 + wall.tv_nsec/1e6,
           (void *)p, ++p->sequence, (now-p->created)/1e6,
           (now-p->interval_start)/1e6, p->interval_frames, p->frames,
           final ? "true" : "false");
   for (unsigned i = 0; i < KK_PERF_METRIC_COUNT; i++) {
      const struct kk_perf_stat *s = &p->stat[i];
      fprintf(p->log, "%s\"%s\":{\"count\":%"PRIu64",\"total_ms\":%.6f,"
              "\"mean_ms\":%.6f,\"max_ms\":%.6f}", i ? "," : "", names[i],
              s->count, s->total/1e6, s->count ? s->total/1e6/s->count : 0., s->max/1e6);
   }
   fprintf(p->log, "},\"deferred_pending\":%"PRId64",\"deferred_peak\":%"PRIu64
           ",\"compiler_active\":%"PRId64",\"compiler_peak\":%"PRIu64
           ",\"gpu_unavailable\":%"PRIu64,
           p->pending, p->pending_peak, p->compiler_active, p->compiler_peak, p->invalid_gpu);
   fprintf(p->log, ",\"submit_pending\":%"PRId64",\"submit_peak\":%"PRIu64
           ",\"submit_scope\":\"common_submit_list_including_worker_head\","
           "\"compiler_scope\":\"in_progress_calls_not_queued_jobs\","
           "\"gpu_frames\":[", p->submit_pending, p->submit_peak);
   for (unsigned i = 0; i < p->gpu_count; i++) {
      const struct kk_perf_gpu_stat *g = &p->gpu[i];
      fprintf(p->log, "%s{\"submit_frame\":%"PRIu64",\"commits\":%"PRIu64
              ",\"command_buffers\":%"PRIu64",\"sum_ms\":%.6f,\"max_ms\":%.6f}",
              i ? "," : "", g->frame, g->count, g->commands, g->total/1e6, g->max/1e6);
   }
   fprintf(p->log, "]}\n");
   fflush(p->log);
   memset(p->stat, 0, sizeof(p->stat));
   p->gpu_count = 0;
   p->invalid_gpu = 0;
   p->interval_frames = 0;
   p->interval_start = now;
   p->pending_peak = p->pending > 0 ? p->pending : 0;
   p->submit_peak = p->submit_pending > 0 ? p->submit_pending : 0;
   p->compiler_peak = p->compiler_active > 0 ? p->compiler_active : 0;
}

static _Thread_local struct kk_perf_compile_scope *compile_scope;

void
kk_perf_compile_begin(struct kk_perf *p, struct kk_perf_compile_scope *scope)
{
   memset(scope, 0, sizeof(*scope));
   if (!p->compile_details) return;
   scope->perf = p;
   scope->previous = compile_scope;
   pthread_mutex_lock(&p->mutex);
   scope->id = ++p->compile_sequence;
   pthread_mutex_unlock(&p->mutex);
   scope->start = kk_perf_now();
   compile_scope = scope;
}

void
kk_perf_compile_end(struct kk_perf_compile_scope *scope, bool binary, int result)
{
   struct kk_perf *p = scope->perf;
   if (!p) return;
   uint64_t end = kk_perf_now();
   compile_scope = scope->previous;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_compile\":1,\"device\":\"%p\",\"id\":%"PRIu64
           ",\"origin\":\"%s\",\"result\":%d,\"start_ms\":%.6f,\"wall_ms\":%.6f,"
           "\"cpu_spans\":\"nested_caller_thread_wall_ms\",\"metrics\":{",
           (void *)p, scope->id, binary ? "deserialize" : "compile", result,
           (scope->start-p->created)/1e6, (end-scope->start)/1e6);
   for (unsigned i = 0; i < KK_PERF_METRIC_COUNT; i++)
      fprintf(p->log, "%s\"%s\":{\"count\":%"PRIu64",\"total_ms\":%.6f}",
              i ? "," : "", names[i], scope->count[i], scope->ns[i]/1e6);
   fprintf(p->log, "}}\n");
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_msl_source(struct kk_perf *p, unsigned stage, const char *source)
{
   if (!p->compile_details || !source) return;
   size_t bytes = strlen(source);
   blake3_hash hash;
   _mesa_blake3_compute(source, bytes, hash);
   char hex[BLAKE3_OUT_LEN * 2 + 1];
   for (unsigned i = 0; i < BLAKE3_OUT_LEN; i++)
      snprintf(hex + 2*i, 3, "%02x", hash[i]);
   struct kk_perf_compile_scope *scope = compile_scope;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_compile_msl\":1,\"device\":\"%p\",\"id\":%"PRIu64
           ",\"stage\":%u,\"bytes\":%zu,\"blake3\":\"%s\"}\n",
           (void *)p, scope && scope->perf == p ? scope->id : 0,
           stage, bytes, hex);
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_add(struct kk_perf *p, enum kk_perf_metric metric, uint64_t ns)
{
   pthread_mutex_lock(&p->mutex);
   struct kk_perf_stat *s = &p->stat[metric];
   s->count++; s->total += ns;
   for (struct kk_perf_compile_scope *scope = compile_scope; scope; scope = scope->previous) {
      if (scope->perf != p) continue;
      scope->count[metric]++;
      scope->ns[metric] += ns;
   }
   if (ns > s->max) s->max = ns;
   /* Completed CPU work must remain visible before the first present too. */
   uint64_t now = kk_perf_now();
   if (now - p->interval_start >= p->time_interval)
      flush_locked(p, now, false);
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_queue(struct kk_perf *p, int64_t delta)
{
   pthread_mutex_lock(&p->mutex);
   p->pending += delta;
   if (p->pending > 0 && (uint64_t)p->pending > p->pending_peak)
      p->pending_peak = p->pending;
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_submit_queue(struct vk_device *vk, int64_t delta)
{
   struct kk_device *dev = container_of(vk, struct kk_device, vk);
   struct kk_perf *p = dev->perf;
   pthread_mutex_lock(&p->mutex);
   p->submit_pending += delta;
   if (p->submit_pending > 0 && (uint64_t)p->submit_pending > p->submit_peak)
      p->submit_peak = p->submit_pending;
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_compiler(struct kk_perf *p, int64_t delta)
{
   pthread_mutex_lock(&p->mutex);
   p->compiler_active += delta;
   if (p->compiler_active > 0 && (uint64_t)p->compiler_active > p->compiler_peak)
      p->compiler_peak = p->compiler_active;
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_frame(struct kk_perf *p)
{
   uint64_t now = kk_perf_now();
   pthread_mutex_lock(&p->mutex);
   p->frames++; p->interval_frames++;
   if (p->interval_frames >= p->frame_interval || now-p->interval_start >= p->time_interval)
      flush_locked(p, now, false);
   pthread_mutex_unlock(&p->mutex);
}

uint64_t
kk_perf_frame_id(struct kk_perf *p)
{
   pthread_mutex_lock(&p->mutex);
   uint64_t frame = p->frames + 1;
   pthread_mutex_unlock(&p->mutex);
   return frame;
}

void
kk_perf_gpu_frame(struct kk_perf *p, uint64_t frame, double start,
                   double end, uint32_t command_buffers)
{
   pthread_mutex_lock(&p->mutex);
   if (!(isfinite(start) && isfinite(end) && start > 0 && end >= start)) {
      p->invalid_gpu++;
      pthread_mutex_unlock(&p->mutex);
      return;
   }
   unsigned i;
   for (i = 0; i < p->gpu_count; i++) if (p->gpu[i].frame == frame) break;
   if (i == ARRAY_SIZE(p->gpu)) { flush_locked(p, kk_perf_now(), false); i = 0; }
   if (i == p->gpu_count) {
      p->gpu_count++;
      p->gpu[i] = (struct kk_perf_gpu_stat){.frame = frame};
   }
   uint64_t ns = (uint64_t)((end-start)*1e9);
   struct kk_perf_gpu_stat *g = &p->gpu[i];
   g->count++; g->commands += command_buffers; g->total += ns;
   if (ns > g->max) g->max = ns;
   pthread_mutex_unlock(&p->mutex);
}

void kk_perf_gpu(struct kk_perf *p, double start, double end, uint32_t count)
{ kk_perf_gpu_frame(p, kk_perf_frame_id(p), start, end, count); }

void kk_perf_cache_lookup(struct vk_device *vk, bool hit)
{
   struct kk_device *dev = container_of(vk, struct kk_device, vk);
   kk_perf_add(dev->perf, hit ? KK_PERF_CACHE_HIT : KK_PERF_CACHE_MISS, 0);
}

void
kk_perf_destroy(struct kk_perf *p)
{
   if (!p) return;
   pthread_mutex_lock(&p->mutex);
   flush_locked(p, kk_perf_now(), true);
   pthread_mutex_unlock(&p->mutex);
   if (p->close_log) fclose(p->log);
   pthread_mutex_destroy(&p->mutex);
   free(p);
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                uint32_t count,
                                const VkGraphicsPipelineCreateInfo *infos,
                                const VkAllocationCallbacks *alloc,
                                VkPipeline *pipelines)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   return KK_PERF_CALL(dev, KK_PERF_PIPELINE_CREATE,
      dev->vk.dispatch_table.CreateGraphicsPipelines(device, cache, count,
                                                      infos, alloc, pipelines));
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_CreateComputePipelines(VkDevice device, VkPipelineCache cache,
                               uint32_t count,
                               const VkComputePipelineCreateInfo *infos,
                               const VkAllocationCallbacks *alloc,
                               VkPipeline *pipelines)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   return KK_PERF_CALL(dev, KK_PERF_PIPELINE_CREATE,
      dev->vk.dispatch_table.CreateComputePipelines(device, cache, count,
                                                     infos, alloc, pipelines));
}

/* Lifecycle wall times include their nested replay/encoding stages. They
 * close spans around native allocation/reset without adding to those stages. */
static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                            const VkCommandBufferBeginInfo *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   return KK_PERF_CALL(dev, KK_PERF_COMMAND_BEGIN,
      dev->vk.dispatch_table.BeginCommandBuffer(commandBuffer, info));
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   return KK_PERF_CALL(dev, KK_PERF_COMMAND_END,
      dev->vk.dispatch_table.EndCommandBuffer(commandBuffer));
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                            VkCommandBufferResetFlags flags)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   return KK_PERF_CALL(dev, KK_PERF_COMMAND_RESET,
      dev->vk.dispatch_table.ResetCommandBuffer(commandBuffer, flags));
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_ResetCommandPool(VkDevice device, VkCommandPool pool,
                          VkCommandPoolResetFlags flags)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   return KK_PERF_CALL(dev, KK_PERF_COMMAND_POOL_RESET,
      dev->vk.dispatch_table.ResetCommandPool(device, pool, flags));
}

void
kk_perf_init_dispatch(struct kk_device *dev)
{
   if (!dev->perf)
      return;
   dev->exposed_dispatch_table.BeginCommandBuffer = kk_perf_BeginCommandBuffer;
   dev->exposed_dispatch_table.EndCommandBuffer = kk_perf_EndCommandBuffer;
   dev->exposed_dispatch_table.ResetCommandBuffer = kk_perf_ResetCommandBuffer;
   dev->exposed_dispatch_table.ResetCommandPool = kk_perf_ResetCommandPool;
   dev->exposed_dispatch_table.CreateGraphicsPipelines = kk_perf_CreateGraphicsPipelines;
   dev->exposed_dispatch_table.CreateComputePipelines = kk_perf_CreateComputePipelines;
   kk_perf_init_wait_dispatch(dev);
   kk_perf_init_wsi_dispatch(dev);
}
