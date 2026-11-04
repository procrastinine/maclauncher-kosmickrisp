/* SPDX-License-Identifier: MIT */
#ifndef KK_PERF_H
#define KK_PERF_H
#include <stdbool.h>
#include <stdint.h>
#include "util/macros.h"

struct kk_device;
struct vk_device;
struct vk_queue_submit;
struct kk_perf;
struct kk_image;
struct kk_cmd_buffer;
struct VkImageCreateInfo;
struct VkRect2D;
enum kk_perf_metric {
   KK_PERF_RECORD, KK_PERF_REPLAY, KK_PERF_ENCODE, KK_PERF_SUBMIT,
   KK_PERF_ACQUIRE, KK_PERF_PRESENT, KK_PERF_PRESENT_WAIT,
   KK_PERF_FENCE_WAIT, KK_PERF_SEMAPHORE_WAIT, KK_PERF_SYNC_WAIT,
   KK_PERF_QUEUE_WAIT, KK_PERF_DRAWABLE_WAIT, KK_PERF_DEFERRED,
   KK_PERF_PIPELINE_CREATE, KK_PERF_SHADER_COMPILE, KK_PERF_NIR,
   KK_PERF_MSL, KK_PERF_METAL_LIBRARY, KK_PERF_METAL_PIPELINE,
   KK_PERF_CACHE_HIT, KK_PERF_CACHE_MISS, KK_PERF_CACHE_BINARY_HIT,
   KK_PERF_METAL_LIBRARY_PAIR,
   KK_PERF_COMMAND_BO_ALLOCATE, KK_PERF_COMMAND_BO_REUSE,
   KK_PERF_EXECUTION_RESIDENCY_COMMIT,
   KK_PERF_COMMAND_BEGIN, KK_PERF_COMMAND_END,
   KK_PERF_COMMAND_RESET, KK_PERF_COMMAND_POOL_RESET,
   KK_PERF_SAMPLED_VIEW_CREATE, KK_PERF_SAMPLED_VIEW_GET,
   KK_PERF_ENCODER_SETUP, KK_PERF_ENCODER_STATE,
   KK_PERF_ENCODER_DRAW, KK_PERF_ENCODER_END,
   KK_PERF_RENDER_ENCODER_BEGIN, KK_PERF_COMPUTE_ENCODER_BEGIN,
   KK_PERF_SETUP_DESCRIPTOR, KK_PERF_SETUP_ENCODER, KK_PERF_SETUP_BINDS,
   KK_PERF_SETUP_BIND_SKIPPED, KK_PERF_DESCRIPTOR_REUSE,
   /* A compute encoder closed an open render encoder; the unroll subset;
    * direct fans converted to triangle lists on the CPU. */
   KK_PERF_RENDER_SPLIT, KK_PERF_RENDER_SPLIT_UNROLL, KK_PERF_FAN_LIST,
   KK_PERF_METRIC_COUNT
};

/* Diagnostic scopes are stack-owned and nest on the calling thread. They do
 * not include work performed on another thread (for example Metal callbacks). */
struct kk_perf_compile_scope {
   struct kk_perf_compile_scope *previous;
   struct kk_perf *perf;
   uint64_t id, start, request_id, thread_id, parent_id;
   bool asynchronous, cancelled;
   uint32_t input_count, stage_mask;
   uint64_t ns[KK_PERF_METRIC_COUNT];
   uint64_t count[KK_PERF_METRIC_COUNT];
};
void kk_perf_compile_begin(struct kk_perf *, struct kk_perf_compile_scope *);
void kk_perf_compile_end(struct kk_perf_compile_scope *, bool binary, int result);
void kk_perf_compile_parent(struct kk_perf *, uint64_t *request, uint64_t *parent);
void kk_perf_async_compute(struct kk_perf *, int queued_delta, int active_delta);
void kk_perf_async_join(struct kk_perf *, uint64_t parent, const char *reason, const char *mode,
                        uint64_t start, uint64_t duration);
void kk_perf_msl_source(struct kk_perf *, unsigned stage, const char *source);

struct kk_perf *kk_perf_create(void);
void kk_perf_gpu_init(struct kk_device *);
void kk_perf_gpu_attach_command(struct kk_perf *, void *command);
void kk_perf_destroy(struct kk_perf *perf);
void kk_perf_metal_archive(struct kk_perf *, void *compiler);
void kk_perf_bind_metal_compiler(struct kk_perf *, void *compiler);
uint64_t kk_perf_now(void);
void kk_perf_add(struct kk_perf *, enum kk_perf_metric, uint64_t ns);
void kk_perf_queue(struct kk_perf *, int64_t delta);
void kk_perf_submit_queue(struct vk_device *, int64_t delta);
void kk_perf_compiler(struct kk_perf *, int64_t delta);
void kk_perf_frame(struct kk_perf *);
uint64_t kk_perf_frame_id(struct kk_perf *);
void kk_perf_gpu_frame(struct kk_perf *, uint64_t frame, double start,
                       double end, uint32_t command_buffers);
void kk_perf_gpu(struct kk_perf *, double start, double end, uint32_t count);
/* Called only by tracking-selected submit dispatch and the existing tracked
 * commit branch. Queue/command addresses are identities, never dereferenced
 * after a completion callback may retire their native storage. */
/* Enabled once at creation by KK_PERF_TRACKING_SUBMITS=1, in addition to
 * ordinary tracking. Interval metrics do not imply per-boundary tracing. */
bool kk_perf_submit_details(const struct kk_perf *);
uint64_t kk_perf_sync_trace(struct kk_perf *, const char *phase, uint64_t id,
   const void *sync, const void *native, uint64_t value, unsigned flags, int result);
uint64_t kk_perf_submit_trace(struct kk_perf *, const char *phase, uint64_t id,
   const void *queue, const struct vk_queue_submit *, int result);
uint64_t kk_perf_commit_trace(struct kk_perf *, const char *phase, uint64_t id,
   const void *queue, const void *submit, const void *command,
   const void *native, uint32_t count, int result);
void kk_perf_cache_lookup(struct vk_device *, bool hit);
/* Physical native pass counters are CPU recording observations, not GPU
 * presentation counts. No bridge snapshot is made when tracking is disabled. */
void kk_perf_render_begin(struct kk_perf *, const void *encoder, void *descriptor,
                           const struct VkRect2D *render_area, const void *command);
void kk_perf_render_stores(struct kk_perf *, const void *encoder,
                            const uint32_t stores[10]);
void kk_perf_render_end(struct kk_perf *, const void *encoder);
/* Opt-in native barrier trace. Frame IDs are CPU present-boundary observations,
 * not measured GPU frame IDs. Native calls retain their exact arguments. */
void kk_perf_barrier(struct kk_perf *, const void *command, const void *encoder,
                     const char *kind, unsigned after, unsigned before,
                     unsigned visibility, const char *site, unsigned line);
#define KK_CMD_BARRIER(CMD, CALL, ENCODER, AFTER, BEFORE, VISIBILITY)             \
   do {                                                                        \
      struct kk_cmd_buffer *_kk_cmd = (CMD);                                   \
      void *_kk_encoder = (ENCODER);                                           \
      enum mtl_stages _kk_after = (AFTER), _kk_before = (BEFORE);               \
      enum mtl_visibility_options _kk_visibility = (VISIBILITY);               \
      CALL(_kk_encoder, _kk_after, _kk_before, _kk_visibility);                 \
      struct kk_perf *_kk_perf = kk_cmd_buffer_device(_kk_cmd)->perf;           \
      if (unlikely(_kk_perf != NULL))                                          \
         kk_perf_barrier(_kk_perf, _kk_cmd->metal.cmd_buf, _kk_encoder,          \
                          #CALL, _kk_after, _kk_before, _kk_visibility,          \
                          __func__, __LINE__);                                 \
   } while (0)
void kk_perf_image_create(struct kk_perf *, const struct kk_image *,
                           const struct VkImageCreateInfo *);
void kk_perf_image_bind(struct kk_perf *, const struct kk_image *, uint32_t plane);
void kk_perf_init_dispatch(struct kk_device *);
void kk_perf_init_wait_dispatch(struct kk_device *);
void kk_perf_init_wsi_dispatch(struct kk_device *);

/* Exclusive encoder-recording scopes. Children (including nested scopes of
 * the same stage) are excluded, as is their diagnostic delivery overhead.
 * Scope counts are entries; encoder counts use the separate BEGIN metrics.
 * Disabled scopes do not touch clocks, TLS, locks or the collector. */
struct kk_perf_encoder_scope {
   struct kk_perf *perf;
   struct kk_perf_encoder_scope *previous;
   uint64_t start, nested;
   enum kk_perf_metric metric;
};
void kk_perf_encoder_scope_begin(struct kk_perf_encoder_scope *,
                                  enum kk_perf_metric);
void kk_perf_encoder_scope_end(struct kk_perf_encoder_scope *);
static inline void
kk_perf_encoder_scope_cleanup(struct kk_perf_encoder_scope *scope)
{
   if (unlikely(scope->perf != NULL))
      kk_perf_encoder_scope_end(scope);
}
#define KK_PERF_ENCODER_SCOPE(dev, stage, name) \
   struct kk_perf_encoder_scope name \
      __attribute__((cleanup(kk_perf_encoder_scope_cleanup))); \
   name.perf = (dev)->perf; \
   if (unlikely(name.perf != NULL)) \
      kk_perf_encoder_scope_begin(&name, stage)

/* The expression runs once. Disabled tracking has one branch and no clock,
 * lock, atomic operation or allocation. Both sides preserve return values. */
#define KK_PERF_CALL(dev, metric, expression) ({ \
   struct kk_perf *_kk_perf = (dev)->perf; \
   __typeof__(expression) _kk_result; \
   if (unlikely(_kk_perf != NULL)) { \
      uint64_t _kk_start = kk_perf_now(); \
      _kk_result = (expression); \
      kk_perf_add(_kk_perf, (metric), kk_perf_now() - _kk_start); \
   } else { \
      _kk_result = (expression); \
   } \
   _kk_result; \
})

#define KK_PERF_VOID(dev, metric, expression) do { \
   struct kk_perf *_kk_perf = (dev)->perf; \
   if (unlikely(_kk_perf != NULL)) { \
      uint64_t _kk_start = kk_perf_now(); \
      expression; \
      kk_perf_add(_kk_perf, (metric), kk_perf_now() - _kk_start); \
   } else { \
      expression; \
   } \
} while (0)
#endif
