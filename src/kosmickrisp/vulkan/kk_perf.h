/* SPDX-License-Identifier: MIT */
#ifndef KK_PERF_H
#define KK_PERF_H
#include <stdbool.h>
#include <stdint.h>
#include "util/macros.h"

struct kk_device;
struct vk_device;
struct kk_perf;
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
   KK_PERF_METRIC_COUNT
};

/* Diagnostic scopes are stack-owned and nest on the calling thread. They do
 * not include work performed on another thread (for example Metal callbacks). */
struct kk_perf_compile_scope {
   struct kk_perf_compile_scope *previous;
   struct kk_perf *perf;
   uint64_t id, start, request_id, thread_id;
   uint32_t input_count, stage_mask;
   uint64_t ns[KK_PERF_METRIC_COUNT];
   uint64_t count[KK_PERF_METRIC_COUNT];
};
void kk_perf_compile_begin(struct kk_perf *, struct kk_perf_compile_scope *);
void kk_perf_compile_end(struct kk_perf_compile_scope *, bool binary, int result);
void kk_perf_msl_source(struct kk_perf *, unsigned stage, const char *source);

struct kk_perf *kk_perf_create(void);
void kk_perf_destroy(struct kk_perf *perf);
void kk_perf_metal_archive(struct kk_perf *, void *compiler);
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
void kk_perf_cache_lookup(struct vk_device *, bool hit);
/* Physical native pass counters are CPU recording observations, not GPU
 * presentation counts. No bridge snapshot is made when tracking is disabled. */
void kk_perf_render_begin(struct kk_perf *, const void *encoder, void *descriptor);
void kk_perf_render_stores(struct kk_perf *, const void *encoder,
                            const uint32_t stores[10]);
void kk_perf_render_end(struct kk_perf *, const void *encoder);
void kk_perf_init_dispatch(struct kk_device *);
void kk_perf_init_wait_dispatch(struct kk_device *);
void kk_perf_init_wsi_dispatch(struct kk_device *);

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
