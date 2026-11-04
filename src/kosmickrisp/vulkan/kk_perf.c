#include "kosmickrisp/bridge/mtl_gpu_timing.h"
/* SPDX-License-Identifier: MIT */
#include "kk_perf.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kk_device.h"
#include "kk_cmd_buffer.h"
#include "kk_image.h"
#include "kosmickrisp/bridge/mtl_texture.h"
#include "vk_util.h"
#include "vk_queue.h"
#include "vk_sync.h"
#include "kosmickrisp/bridge/mtl_render_state.h"
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
#define KK_PERF_MAX_RENDER_PASSES 256
#define KK_PERF_MAX_ATTACHMENT_STATS 512

struct kk_perf_render_pass {
   const void *encoder;
   struct mtl_render_pass_info info;
   VkRect2D area;
   bool area_known;
   const void *command;
   uint64_t trace_id;
};
struct kk_perf_attachment_stat {
   uint32_t aspect, format, samples, load, store;
   uint64_t count;
   const void *texture, *base_texture;
   VkRect2D area;
   uint32_t width, height;
   bool area_known;
};

struct kk_gpu_stats;
struct kk_perf {
   struct kk_gpu_stats *gpu_encoders;
   void *gpu_context;
   pthread_mutex_t mutex;
   FILE *log;
   void *metal_compiler;
   bool close_log, compile_details, submit_details, barrier_details;
   bool device_features_known;
   bool pipeline_statistics_query, writable_msaa, dynamic_rendering;
   uint64_t compile_sequence, pipeline_sequence, submit_sequence;
   uint64_t barrier_sequence, render_trace_sequence;
   uint64_t created, interval_start, frames, interval_frames;
   uint64_t frame_interval, time_interval, sequence;
   int64_t pending, submit_pending, compiler_active;
   uint64_t pending_peak, submit_peak, compiler_peak, invalid_gpu;
   int64_t async_compute_queued, async_compute_active;
   uint64_t async_compute_peak;
   struct kk_perf_stat stat[KK_PERF_METRIC_COUNT];
   struct kk_perf_gpu_stat gpu[256];
   unsigned gpu_count;
   struct kk_perf_render_pass render_active[KK_PERF_MAX_RENDER_PASSES];
   struct kk_perf_attachment_stat render_attachments[KK_PERF_MAX_ATTACHMENT_STATS];
   unsigned render_attachment_count;
   uint64_t render_count, render_samples[8];
   uint64_t render_dropped_passes, render_dropped_attachments;
};

#include "kk_gpu_stats.h"

static const char *const names[KK_PERF_METRIC_COUNT] = {
   "record", "replay", "encode", "submit", "acquire", "present",
   "present_wait", "fence_wait", "semaphore_wait", "sync_wait",
   "queue_wait", "drawable_wait", "deferred", "pipeline_create",
   "shader_compile", "nir", "msl", "metal_library", "metal_pipeline",
   "cache_hit", "cache_miss", "cache_binary_hit", "metal_library_pair",
   "command_bo_allocate", "command_bo_reuse", "execution_residency_commit",
   "command_begin", "command_end", "command_reset", "command_pool_reset",
   "sampled_view_create", "sampled_view_get",
   "encoder_setup", "encoder_state", "encoder_draw", "encoder_end",
   "render_encoder_begin", "compute_encoder_begin",
   "setup_descriptor", "setup_encoder", "setup_binds",
   "setup_bind_skipped", "descriptor_reuse",
   "render_split", "render_split_unroll", "fan_list"
};

uint64_t kk_perf_now(void) { return os_time_get_nano(); }

static void flush_locked(struct kk_perf *, uint64_t, bool);
static void render_flush_locked(struct kk_perf *);

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
   const char *submits = getenv("KK_PERF_TRACKING_SUBMITS");
   p->submit_details = submits && !strcmp(submits, "1");
   const char *barriers = getenv("KK_PERF_TRACKING_BARRIERS");
   p->barrier_details = barriers && !strcmp(barriers, "1");
   const char *gpu = getenv("KK_PERF_TRACKING_GPU_ENCODERS");
   if (gpu && !strcmp(gpu, "1")) p->gpu_encoders = calloc(1, sizeof(*p->gpu_encoders));
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

bool
kk_perf_submit_details(const struct kk_perf *p)
{
   return p && p->submit_details;
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
   if (p->device_features_known)
      fprintf(p->log, "},\"device_features\":{\"pipeline_statistics_query\":%s,"
              "\"shader_storage_image_multisample\":%s,\"dynamic_rendering\":%s}",
              p->pipeline_statistics_query ? "true" : "false",
              p->writable_msaa ? "true" : "false",
              p->dynamic_rendering ? "true" : "false");
   else
      fprintf(p->log, "},\"device_features\":null");
   fprintf(p->log, ",\"deferred_pending\":%"PRId64",\"deferred_peak\":%"PRIu64
           ",\"compiler_active\":%"PRId64",\"compiler_peak\":%"PRIu64
           ",\"gpu_unavailable\":%"PRIu64,
           p->pending, p->pending_peak, p->compiler_active, p->compiler_peak, p->invalid_gpu);
   fprintf(p->log, ",\"async_compute_queued\":%"PRId64",\"async_compute_active\":%"PRId64
           ",\"async_compute_peak\":%"PRIu64,
           p->async_compute_queued, p->async_compute_active, p->async_compute_peak);
   struct mtl_compiler_cache_stats archive;
   if (p->metal_compiler && mtl_compiler_get_cache_stats(p->metal_compiler, &archive))
      fprintf(p->log, ",\"metal_archive\":{\"scope\":\"shared_compiler_lifetime\","
         "\"compiler\":\"%p\",\"archive_loaded\":%s,\"requests\":%"PRIu64",\"lookups\":%"PRIu64
         ",\"piece_loads\":%"PRIu64",\"piece_load_ms\":%.6f,\"archive_opens\":%"PRIu64
         ",\"archive_open_ms\":%.6f,\"hits\":%"PRIu64",\"native_pipeline_successes\":%"PRIu64
         ",\"checkpoints\":%"PRIu64",\"store_requested_bytes\":%"PRIu64
         ",\"snapshot_ms\":%.6f,\"capture_wait_ms\":%.6f"
         ",\"vertex_library_requests\":%"PRIu64",\"vertex_library_hits\":%"PRIu64"}",
         p->metal_compiler, archive.archive_loaded ? "true" : "false", archive.requests,
         archive.lookups, archive.piece_loads, archive.piece_load_ns / 1e6, archive.archive_opens,
         archive.archive_open_ns / 1e6, archive.hits, archive.native_compiles,
         archive.checkpoints,
         archive.stored_bytes, archive.snapshot_ns / 1e6, archive.capture_wait_ns / 1e6,
         archive.vertex_library_requests, archive.vertex_library_hits);
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
   fprintf(p->log, "],\"encoder_stage_scope\":\"exclusive_nested_recording_wall_ms\"");
   render_flush_locked(p);
   gpu_encoders_flush_locked(p);
   fprintf(p->log, "}\n");
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

struct kk_perf_pipeline_scope {
   struct kk_perf_pipeline_scope *previous;
   struct kk_perf *perf;
   uint64_t id, start, thread;
   uint32_t count;
   const char *kind;
};
static _Thread_local struct kk_perf_pipeline_scope *pipeline_scope;

static uint64_t
kk_perf_thread_id(void)
{
#ifdef __APPLE__
   uint64_t id = 0;
   pthread_threadid_np(NULL, &id);
   return id;
#else
   return (uintptr_t)pthread_self();
#endif
}

static void
kk_perf_pipeline_begin(struct kk_perf *p, struct kk_perf_pipeline_scope *scope,
                       const char *kind, uint32_t count)
{
   *scope = (struct kk_perf_pipeline_scope) {
      .previous = pipeline_scope, .perf = p, .start = kk_perf_now(),
      .thread = kk_perf_thread_id(), .count = count, .kind = kind,
   };
   pthread_mutex_lock(&p->mutex);
   scope->id = ++p->pipeline_sequence;
   fprintf(p->log, "{\"kk_pipeline_create\":1,\"device\":\"%p\",\"request_id\":%"PRIu64
      ",\"phase\":\"begin\",\"kind\":\"%s\",\"thread\":%"PRIu64",\"count\":%u,\"start_ms\":%.6f}\n",
      (void *)p, scope->id, kind, scope->thread, count, (scope->start - p->created) / 1e6);
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
   pipeline_scope = scope;
}

static void
kk_perf_pipeline_end(struct kk_perf_pipeline_scope *scope, VkResult result)
{
   uint64_t end = kk_perf_now();
   struct kk_perf *p = scope->perf;
   pipeline_scope = scope->previous;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_pipeline_create\":1,\"device\":\"%p\",\"request_id\":%"PRIu64
      ",\"phase\":\"end\",\"kind\":\"%s\",\"thread\":%"PRIu64",\"count\":%u,\"result\":%d,\"wall_ms\":%.6f}\n",
      (void *)p, scope->id, scope->kind, scope->thread, scope->count, result, (end - scope->start) / 1e6);
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_image_create(struct kk_perf *p, const struct kk_image *image,
                      const VkImageCreateInfo *info)
{
   const VkImageFormatListCreateInfo *formats =
      vk_find_struct_const(info->pNext, IMAGE_FORMAT_LIST_CREATE_INFO);
   const double start_ms = (kk_perf_now() - p->created) / 1e6;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_image\":1,\"event\":\"create\",\"device\":\"%p\","
           "\"image\":\"%p\",\"start_ms\":%.6f,\"vk_format\":%u,"
           "\"application_usage\":%"PRIu64",\"vk_usage\":%"PRIu64","
           "\"vk_flags\":%"PRIu64",\"width\":%u,\"height\":%u,\"depth\":%u,"
           "\"samples\":%u,\"format_list_present\":%s,\"view_formats\":[",
           (void *)p, (const void *)image, start_ms, image->vk.format,
           (uint64_t)image->application_usage, (uint64_t)image->vk.usage,
           (uint64_t)image->vk.create_flags, image->vk.extent.width,
           image->vk.extent.height, image->vk.extent.depth, image->vk.samples,
           formats ? "true" : "false");
   for (uint32_t i = 0; formats && i < formats->viewFormatCount; ++i)
      fprintf(p->log, "%s%u", i ? "," : "", formats->pViewFormats[i]);
   fprintf(p->log, "],\"planes\":[");
   for (uint32_t i = 0; i < image->plane_count; ++i) {
      const struct kk_image_layout *layout = &image->planes[i].layout;
      fprintf(p->log, "%s{\"plane\":%u,\"metal_format\":%u,\"metal_usage\":%u,"
              "\"optimized_layout\":%s,\"linear\":%s,\"storage_mode\":null}",
              i ? "," : "", i, layout->format.mtl, (unsigned)layout->usage,
              layout->optimized_layout ? "true" : "false",
              layout->linear ? "true" : "false");
   }
   fprintf(p->log, "]}\n");
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_image_bind(struct kk_perf *p, const struct kk_image *image, uint32_t plane)
{
   struct mtl_texture_info info;
   mtl_texture_get_info(image->planes[plane].mtl_handle, &info);
   const double start_ms = (kk_perf_now() - p->created) / 1e6;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_image\":1,\"event\":\"bind\",\"device\":\"%p\","
           "\"image\":\"%p\",\"plane\":%u,\"start_ms\":%.6f,"
           "\"metal_format\":%u,\"metal_usage\":%u,\"storage_mode\":%u,"
           "\"samples\":%u,\"texture\":\"%p\",\"base_texture\":\"%p\"}\n",
           (void *)p, (const void *)image, plane, start_ms, info.format, info.usage,
           info.storage_mode, info.samples,
           (const void *)image->planes[plane].mtl_handle, info.base_texture);
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
}

static unsigned
render_sample_bucket(uint32_t samples)
{
   switch (samples) {
   case 1: return 0;
   case 2: return 1;
   case 4: return 2;
   case 8: return 3;
   case 16: return 4;
   case 32: return 5;
   case 64: return 6;
   default: return 7;
   }
}

static void
render_flush_locked(struct kk_perf *p)
{
   unsigned active = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(p->render_active); ++i)
      active += p->render_active[i].encoder != NULL;
   fprintf(p->log, ",\"render_passes\":{\"scope\":\"native_encoder_close_CPU_recording\","
           "\"count\":%"PRIu64",\"active\":%u,\"dropped_passes\":%"PRIu64
           ",\"dropped_attachments\":%"PRIu64",\"pass_max_sample_counts\":{",
           p->render_count, active, p->render_dropped_passes,
           p->render_dropped_attachments);
   static const char *const samples[] = {"1", "2", "4", "8", "16", "32", "64", "other"};
   for (unsigned i = 0; i < ARRAY_SIZE(samples); ++i)
      fprintf(p->log, "%s\"%s\":%"PRIu64, i ? "," : "", samples[i], p->render_samples[i]);
   fprintf(p->log, "},\"attachments\":[");
   static const char *const aspects[] = {"color", "depth", "stencil"};
   for (unsigned i = 0; i < p->render_attachment_count; ++i) {
      const struct kk_perf_attachment_stat *a = &p->render_attachments[i];
      fprintf(p->log, "%s{\"aspect\":\"%s\",\"metal_format\":%u,\"samples\":%u,"
              "\"load_action\":%u,\"store_action\":%u,\"count\":%"PRIu64
              ",\"texture\":\"%p\",\"base_texture\":\"%p\","
              "\"target_extent\":[%u,%u],\"render_area\":",
              i ? "," : "", aspects[a->aspect], a->format, a->samples,
              a->load, a->store, a->count, a->texture, a->base_texture,
              a->width, a->height);
      if (a->area_known)
         fprintf(p->log, "{\"offset\":[%d,%d],\"extent\":[%u,%u]}",
                 a->area.offset.x, a->area.offset.y,
                 a->area.extent.width, a->area.extent.height);
      else
         fprintf(p->log, "null");
      fprintf(p->log, "}");
   }
   fprintf(p->log, "]}");
   p->render_count = p->render_dropped_passes = p->render_dropped_attachments = 0;
   p->render_attachment_count = 0;
   memset(p->render_samples, 0, sizeof(p->render_samples));
}

void
kk_perf_render_begin(struct kk_perf *p, const void *encoder, void *descriptor,
                     const VkRect2D *render_area, const void *command)
{
   if (!encoder) {
      pthread_mutex_lock(&p->mutex);
      p->render_dropped_passes++;
      pthread_mutex_unlock(&p->mutex);
      return;
   }
   struct mtl_render_pass_info info;
   mtl_render_pass_descriptor_get_info(descriptor, &info);
   pthread_mutex_lock(&p->mutex);
   unsigned free_slot = ARRAY_SIZE(p->render_active);
   for (unsigned i = 0; i < ARRAY_SIZE(p->render_active); ++i) {
      if (p->render_active[i].encoder == encoder) {
         p->render_dropped_passes++;
         free_slot = i;
         break;
      }
      if (!p->render_active[i].encoder && free_slot == ARRAY_SIZE(p->render_active))
         free_slot = i;
   }
   if (free_slot == ARRAY_SIZE(p->render_active))
      p->render_dropped_passes++;
   else {
      struct kk_perf_render_pass *pass = &p->render_active[free_slot];
      *pass = (struct kk_perf_render_pass) {
         .encoder = encoder, .info = info,
         .area = render_area ? *render_area : (VkRect2D){0},
         .area_known = render_area != NULL, .command = command,
      };
      if (p->barrier_details) {
         pass->trace_id = ++p->render_trace_sequence;
         fprintf(p->log, "{\"kk_render_pass\":1,\"phase\":\"begin\",\"device\":\"%p\","
                 "\"pass_id\":%"PRIu64",\"cpu_frame\":%"PRIu64",\"command\":\"%p\","
                 "\"encoder\":\"%p\",\"target_extent\":[%u,%u],\"attachments\":[",
                 (void *)p, pass->trace_id, p->frames + 1, command, encoder,
                 info.width, info.height);
         bool comma = false;
         for (unsigned a = 0; a < MTL_RENDER_PASS_INFO_ATTACHMENTS; ++a) {
            const struct mtl_render_pass_attachment_info *t = &info.attachments[a];
            if (!t->present) continue;
            fprintf(p->log, "%s{\"slot\":%u,\"texture\":\"%p\",\"base_texture\":\"%p\","
                    "\"load\":%u,\"store\":%u}", comma ? "," : "", a,
                    t->texture, t->base_texture, t->load, t->store);
            comma = true;
         }
         fprintf(p->log, "]}\n");
      }
   }
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_barrier(struct kk_perf *p, const void *command, const void *encoder,
                 const char *kind, unsigned after, unsigned before,
                 unsigned visibility, const char *site, unsigned line)
{
   if (!p->barrier_details)
      return;
   pthread_mutex_lock(&p->mutex);
   uint64_t pass_id = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(p->render_active); ++i)
      if (p->render_active[i].encoder == encoder) {
         pass_id = p->render_active[i].trace_id;
         break;
      }
   fprintf(p->log, "{\"kk_barrier\":1,\"device\":\"%p\",\"sequence\":%"PRIu64
           ",\"cpu_frame\":%"PRIu64",\"command\":\"%p\",\"encoder\":\"%p\","
           "\"pass_id\":%"PRIu64",\"kind\":\"%s\",\"after_stages\":%u,"
           "\"before_stages\":%u,\"visibility\":%u,\"site\":\"%s\",\"line\":%u}\n",
           (void *)p, ++p->barrier_sequence, p->frames + 1, command, encoder,
           pass_id, kind, after, before, visibility, site, line);
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_render_stores(struct kk_perf *p, const void *encoder,
                       const uint32_t stores[10])
{
   if (!encoder)
      return;
   pthread_mutex_lock(&p->mutex);
   for (unsigned i = 0; i < ARRAY_SIZE(p->render_active); ++i) {
      struct kk_perf_render_pass *pass = &p->render_active[i];
      if (pass->encoder != encoder)
         continue;
      for (unsigned a = 0; a < MTL_RENDER_PASS_INFO_ATTACHMENTS; ++a)
         if (stores[a] != UINT32_MAX)
            pass->info.attachments[a].store = (enum mtl_store_action)stores[a];
      break;
   }
   pthread_mutex_unlock(&p->mutex);
}

void
kk_perf_render_end(struct kk_perf *p, const void *encoder)
{
   if (!encoder)
      return;
   pthread_mutex_lock(&p->mutex);
   for (unsigned i = 0; i < ARRAY_SIZE(p->render_active); ++i) {
      struct kk_perf_render_pass *pass = &p->render_active[i];
      if (pass->encoder != encoder)
         continue;
      uint32_t samples = 0;
      p->render_count++;
      for (unsigned a = 0; a < MTL_RENDER_PASS_INFO_ATTACHMENTS; ++a) {
         const struct mtl_render_pass_attachment_info *info = &pass->info.attachments[a];
         if (!info->present)
            continue;
         samples = MAX2(samples, info->samples);
         const uint32_t aspect = a < 8 ? 0 : a == 8 ? 1 : 2;
         unsigned k;
         for (k = 0; k < p->render_attachment_count; ++k) {
            struct kk_perf_attachment_stat *s = &p->render_attachments[k];
            if (s->aspect == aspect && s->format == info->format &&
                s->samples == info->samples && s->load == (uint32_t)info->load &&
                s->store == (uint32_t)info->store &&
                s->texture == info->texture && s->base_texture == info->base_texture &&
                s->width == pass->info.width && s->height == pass->info.height &&
                s->area_known == pass->area_known &&
                s->area.offset.x == pass->area.offset.x &&
                s->area.offset.y == pass->area.offset.y &&
                s->area.extent.width == pass->area.extent.width &&
                s->area.extent.height == pass->area.extent.height) {
               s->count++;
               break;
            }
         }
         if (k == p->render_attachment_count) {
            if (k == ARRAY_SIZE(p->render_attachments))
               p->render_dropped_attachments++;
            else {
               p->render_attachments[k] = (struct kk_perf_attachment_stat){
                  .aspect = aspect, .format = info->format,
                  .samples = info->samples, .load = info->load, .store = info->store,
                  .count = 1, .texture = info->texture,
                  .base_texture = info->base_texture, .area = pass->area,
                  .width = pass->info.width, .height = pass->info.height,
                  .area_known = pass->area_known};
               p->render_attachment_count++;
            }
         }
      }
      if (!samples)
         samples = pass->info.default_samples;
      p->render_samples[render_sample_bucket(samples)]++;
      if (p->barrier_details)
         fprintf(p->log, "{\"kk_render_pass\":1,\"phase\":\"end\",\"device\":\"%p\","
                 "\"pass_id\":%"PRIu64",\"cpu_frame\":%"PRIu64",\"command\":\"%p\","
                 "\"encoder\":\"%p\"}\n", (void *)p, pass->trace_id,
                 p->frames + 1, pass->command, encoder);
      pass->encoder = NULL;
      break;
   }
   pthread_mutex_unlock(&p->mutex);
}

static _Thread_local struct kk_perf_encoder_scope *encoder_scope;

void
kk_perf_encoder_scope_begin(struct kk_perf_encoder_scope *scope,
                             enum kk_perf_metric metric)
{
   scope->previous = encoder_scope;
   scope->metric = metric;
   scope->nested = 0;
   scope->start = kk_perf_now();
   encoder_scope = scope;
}

void
kk_perf_encoder_scope_end(struct kk_perf_encoder_scope *scope)
{
   uint64_t elapsed = kk_perf_now() - scope->start;
   encoder_scope = scope->previous;
   kk_perf_add(scope->perf, scope->metric, elapsed - scope->nested);
   if (scope->previous)
      scope->previous->nested += kk_perf_now() - scope->start;
}

static _Thread_local struct kk_perf_compile_scope *compile_scope;

void
kk_perf_compile_parent(struct kk_perf *p, uint64_t *request, uint64_t *parent)
{
   *request = *parent = 0;
   if (compile_scope && compile_scope->perf == p) {
      *request = compile_scope->request_id;
      *parent = compile_scope->id;
   }
}

void
kk_perf_async_compute(struct kk_perf *p, int queued_delta, int active_delta)
{
   pthread_mutex_lock(&p->mutex);
   p->async_compute_queued += queued_delta;
   p->async_compute_active += active_delta;
   if (p->async_compute_active > 0 &&
       (uint64_t)p->async_compute_active > p->async_compute_peak)
      p->async_compute_peak = p->async_compute_active;
   pthread_mutex_unlock(&p->mutex);
}


void
kk_perf_async_join(struct kk_perf *p, uint64_t parent, const char *reason,
                    uint64_t start, uint64_t duration)
{
   if (!p->compile_details) return;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_async_join\":1,\"device\":\"%p\",\"parent_id\":%"PRIu64
      ",\"reason\":\"%s\",\"thread\":%"PRIu64",\"start_ms\":%.6f,\"wall_ms\":%.6f}\n",
      (void *)p, parent, reason, kk_perf_thread_id(), (start - p->created) / 1e6, duration / 1e6);
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
}

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
   scope->thread_id = kk_perf_thread_id();
   scope->request_id = pipeline_scope && pipeline_scope->perf == p ? pipeline_scope->id : 0;
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
           ",\"request_id\":%"PRIu64",\"thread\":%"PRIu64",\"input_count\":%u,\"stage_mask\":%u,"
           "\"origin\":\"%s\",\"asynchronous\":%s,\"cancelled\":%s,\"parent_id\":%"PRIu64",\"result\":%d,\"start_ms\":%.6f,\"wall_ms\":%.6f,"
           "\"cpu_spans\":\"nested_caller_thread_wall_ms\",\"metrics\":{",
           (void *)p, scope->id, scope->request_id, scope->thread_id, scope->input_count, scope->stage_mask, binary ? "deserialize" : "compile", scope->asynchronous ? "true" : "false", scope->cancelled ? "true" : "false", scope->parent_id, result,
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
   mtl_release(p->metal_compiler);
   if (p->close_log) fclose(p->log);
   pthread_mutex_destroy(&p->mutex);
   free(p->gpu_encoders);
   mtl_release(p->gpu_context);
   free(p);
}

static ATTRIBUTE_NOINLINE VkResult
kk_perf_CreateGraphicsPipelines_detailed(VkDevice device, VkPipelineCache cache,
   uint32_t count, const VkGraphicsPipelineCreateInfo *infos,
   const VkAllocationCallbacks *alloc, VkPipeline *pipelines)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_perf_pipeline_scope scope;
   kk_perf_pipeline_begin(dev->perf, &scope, "graphics", count);
   VkResult result = KK_PERF_CALL(dev, KK_PERF_PIPELINE_CREATE,
      dev->vk.dispatch_table.CreateGraphicsPipelines(device, cache, count, infos, alloc, pipelines));
   kk_perf_pipeline_end(&scope, result);
   return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                uint32_t count,
                                const VkGraphicsPipelineCreateInfo *infos,
                                const VkAllocationCallbacks *alloc,
                                VkPipeline *pipelines)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   if (dev->perf && dev->perf->compile_details)
      return kk_perf_CreateGraphicsPipelines_detailed(device, cache, count, infos, alloc, pipelines);
   return KK_PERF_CALL(dev, KK_PERF_PIPELINE_CREATE,
      dev->vk.dispatch_table.CreateGraphicsPipelines(device, cache, count,
                                                      infos, alloc, pipelines));
}

static ATTRIBUTE_NOINLINE VkResult
kk_perf_CreateComputePipelines_detailed(VkDevice device, VkPipelineCache cache,
   uint32_t count, const VkComputePipelineCreateInfo *infos,
   const VkAllocationCallbacks *alloc, VkPipeline *pipelines)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_perf_pipeline_scope scope;
   kk_perf_pipeline_begin(dev->perf, &scope, "compute", count);
   VkResult result = KK_PERF_CALL(dev, KK_PERF_PIPELINE_CREATE,
      dev->vk.dispatch_table.CreateComputePipelines(device, cache, count, infos, alloc, pipelines));
   kk_perf_pipeline_end(&scope, result);
   return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_CreateComputePipelines(VkDevice device, VkPipelineCache cache,
                               uint32_t count,
                               const VkComputePipelineCreateInfo *infos,
                               const VkAllocationCallbacks *alloc,
                               VkPipeline *pipelines)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   if (dev->perf && dev->perf->compile_details)
      return kk_perf_CreateComputePipelines_detailed(device, cache, count, infos, alloc, pipelines);
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
   /* Cache enabled feature state once; it does not report active queries. */
   pthread_mutex_lock(&dev->perf->mutex);
   dev->perf->pipeline_statistics_query =
      dev->vk.enabled_features.pipelineStatisticsQuery;
   dev->perf->writable_msaa =
      dev->vk.enabled_features.shaderStorageImageMultisample;
   dev->perf->dynamic_rendering = dev->vk.enabled_features.dynamicRendering;
   dev->perf->device_features_known = true;
   pthread_mutex_unlock(&dev->perf->mutex);
   dev->exposed_dispatch_table.BeginCommandBuffer = kk_perf_BeginCommandBuffer;
   dev->exposed_dispatch_table.EndCommandBuffer = kk_perf_EndCommandBuffer;
   dev->exposed_dispatch_table.ResetCommandBuffer = kk_perf_ResetCommandBuffer;
   dev->exposed_dispatch_table.ResetCommandPool = kk_perf_ResetCommandPool;
   dev->exposed_dispatch_table.CreateGraphicsPipelines = kk_perf_CreateGraphicsPipelines;
   dev->exposed_dispatch_table.CreateComputePipelines = kk_perf_CreateComputePipelines;
   kk_perf_init_wait_dispatch(dev);
   kk_perf_init_wsi_dispatch(dev);
}

/* The final client records native-compiler lifetime totals after its flush.
 * Other Vulkan devices may have shared this compiler during the process. */
void
kk_perf_metal_archive(struct kk_perf *p, void *compiler)
{
   if (!p) return;
   struct mtl_compiler_cache_stats stats;
   if (!mtl_compiler_get_cache_stats(compiler, &stats)) return;
   pthread_mutex_lock(&p->mutex);
   fprintf(p->log, "{\"kk_metal_archive\":1,\"device\":\"%p\",\"compiler\":\"%p\","
      "\"archive_loaded\":%s,\"requests\":%"PRIu64",\"lookups\":%"PRIu64
      ",\"piece_loads\":%"PRIu64",\"piece_load_ms\":%.6f,\"archive_opens\":%"PRIu64
      ",\"archive_open_ms\":%.6f,\"hits\":%"PRIu64
      ",\"native_pipeline_successes\":%"PRIu64",\"store_requested_bytes\":%"PRIu64"}\n",
      (void *)p, compiler, stats.archive_loaded ? "true" : "false",
      stats.requests, stats.lookups, stats.piece_loads, stats.piece_load_ns / 1e6,
      stats.archive_opens, stats.archive_open_ns / 1e6, stats.hits, stats.native_compiles,
      stats.stored_bytes);
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
}

/* A tracker owns its native reference only while this Vulkan device is live.
 * Unbind before compiler-table release so workaround11 sees no hidden client. */
void
kk_perf_bind_metal_compiler(struct kk_perf *p, void *compiler)
{
   if (!p) return;
   pthread_mutex_lock(&p->mutex);
   void *old = p->metal_compiler;
   p->metal_compiler = compiler ? mtl_retain(compiler) : NULL;
   pthread_mutex_unlock(&p->mutex);
   mtl_release(old);
}

/* Flush each boundary so an interrupted process retains the last reached
 * seam. Tracking is opt-in; ordinary submissions never enter these helpers. */
uint64_t
kk_perf_submit_trace(struct kk_perf *p, const char *phase, uint64_t id,
   const void *queue, const struct vk_queue_submit *submit, int result)
{
   pthread_mutex_lock(&p->mutex);
   if (!id) id = ++p->submit_sequence;
   fprintf(p->log, "{\"kk_submit_trace\":1,\"id\":%"PRIu64
      ",\"phase\":\"%s\",\"queue\":\"%p\",\"submit\":\"%p\",\"result\":%d"
      ",\"time_ns\":%"PRIu64",\"commands\":[", id, phase, queue,
      (const void *)submit, result, kk_perf_now());
   for (uint32_t i = 0; i < submit->command_buffer_count; i++)
      fprintf(p->log, "%s\"%p\"", i ? "," : "", (void *)submit->command_buffers[i]);
   fprintf(p->log, "],\"waits\":[");
   for (uint32_t i = 0; i < submit->wait_count; i++)
      fprintf(p->log, "%s{\"sync\":\"%p\",\"value\":%"PRIu64"}",
         i ? "," : "", (void *)submit->waits[i].sync, submit->waits[i].wait_value);
   fprintf(p->log, "],\"signals\":[");
   for (uint32_t i = 0; i < submit->signal_count; i++)
      fprintf(p->log, "%s{\"sync\":\"%p\",\"value\":%"PRIu64"}",
         i ? "," : "", (void *)submit->signals[i].sync, submit->signals[i].signal_value);
   fprintf(p->log, "],\"signals_issued\":%s}\n",
      !strcmp(phase, "return") && result == VK_SUCCESS ? "true" : "false");
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
   return id;
}

uint64_t
kk_perf_commit_trace(struct kk_perf *p, const char *phase, uint64_t id,
   const void *queue, const void *submit, const void *command,
   const void *native, uint32_t count, int result)
{
   pthread_mutex_lock(&p->mutex);
   if (!id) id = ++p->submit_sequence;
   fprintf(p->log, "{\"kk_commit_trace\":1,\"id\":%"PRIu64
      ",\"phase\":\"%s\",\"queue\":\"%p\",\"submit\":\"%p\",\"command\":\"%p\","
      "\"native\":\"%p\",\"count\":%u,\"result\":%d,\"time_ns\":%"PRIu64"}\n",
      id, phase, queue, submit, command, native, count, result, kk_perf_now());
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
   return id;
}

uint64_t
kk_perf_sync_trace(struct kk_perf *p, const char *phase, uint64_t id,
   const void *sync, const void *native, uint64_t value, unsigned flags, int result)
{
   pthread_mutex_lock(&p->mutex);
   if (!id) id = ++p->submit_sequence;
   fprintf(p->log, "{\"kk_sync_trace\":1,\"id\":%"PRIu64
      ",\"phase\":\"%s\",\"sync\":\"%p\",\"native\":\"%p\",\"value\":%"PRIu64
      ",\"flags\":%u,\"result\":%d,\"time_ns\":%"PRIu64"}\n",
      id, phase, sync, native, value, flags, result, kk_perf_now());
   fflush(p->log);
   pthread_mutex_unlock(&p->mutex);
   return id;
}
