/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef MTL_COMPILER_H
#define MTL_COMPILER_H 1

#include "mtl_types.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

/* Optional calling-thread compile attribution. Keys are borrowed 32-byte
 * search digests; NULL means no key. No native completion callback is used. */
struct mtl_compile_trace {
   void (*event)(void *data, const char *event, const uint8_t *key,
                 bool specialized);
   void *data;
};
struct mtl_compile_trace mtl_compiler_set_trace(struct mtl_compile_trace trace);

/* Bridge enums */
enum mtl_pixel_format;

mtl_render_pipeline_descriptor *mtl_new_mesh_pipeline_descriptor(
   mtl_function_descriptor *mesh, mtl_function_descriptor *object,
   uint32_t mesh_threads, uint32_t object_threads,
   uint32_t payload_bytes, uint32_t mesh_groups);

/* Compiler */
/* Initialize once before publishing the first compiler. No-op when unset. */
void mtl_init_debug_msl_salt(const char *salt);
mtl_compiler *mtl_new_compiler(mtl_device *device);
/* The cached constructor owns data, including on compiler creation failure.
 * load returns malloc-owned bytes; store copies them before returning. */
struct mtl_compiler_cache_ops {
   bool (*available)(void *data);
   void *(*load)(void *data, size_t *size);
   bool (*store)(void *data, const void *bytes, size_t size);
   void *(*load_piece)(void *data, const uint8_t id[32], size_t *size);
   /* Complete the piece write before allowing an index to reference it. */
   bool (*store_piece)(void *data, const void *bytes, size_t size, uint8_t id[32]);
   void (*store_shader)(void *data, const void *key, size_t key_size,
                        const void *bytes, size_t size);
   void (*destroy)(void *data);
};
mtl_compiler *mtl_new_cached_compiler(mtl_device *device,
   const struct mtl_compiler_cache_ops *ops, void *data);
struct mtl_compiler_cache_stats {
   uint64_t lookups, hits, native_compiles, stored_bytes;
   uint64_t checkpoints, snapshot_ns, capture_wait_ns, requests;
   /* Every MTLLibrary request through the in-process source cache, and the
    * vertex-stage subset of graphics library pairs. */
   uint64_t library_requests, library_hits;
   /* Pipelines compiled on the non-capturing compiler during a snapshot, and
    * those later captured on the capturing compiler. */
   uint64_t bypass_compiles, recaptures;
   uint64_t vertex_library_requests, vertex_library_hits;
   /* Disk-cache piece reads (load_piece) and native archive opens
    * (newArchiveWithURL), with the time spent in each call. */
   uint64_t piece_loads, piece_load_ns, archive_opens, archive_open_ns;
   bool archive_loaded;
};
/* Lifetime totals for this shared native compiler, not one Vulkan device. */
bool mtl_compiler_get_cache_stats(mtl_compiler *compiler,
   struct mtl_compiler_cache_stats *stats);
/* Call only after the final compiler client has drained its queues. */
void mtl_compiler_finish_cache(mtl_compiler *compiler);

/* Capture publication shares read access; only background serializer snapshots
 * take exclusive access. The token is borrowed until the native call returns. */
void *mtl_compiler_cache_begin(mtl_compiler *compiler);
void mtl_compiler_cache_end(void *token);
void mtl_compiler_cache_record_pipeline(mtl_compiler *compiler);
void mtl_compiler_cache_record_descriptor(mtl_compiler *compiler, void *descriptor);
/* Descriptor calls never wait for a snapshot: *native receives the compiler
 * the native call uses, and the descriptor is captured after the snapshot. */
void *mtl_compiler_cache_begin_pipeline(mtl_compiler *compiler, mtl_compiler **native);
void mtl_compiler_cache_record_pipeline_descriptor(mtl_compiler *compiler, void *token,
   void *descriptor);
void mtl_compiler_cache_recapture(mtl_compiler *compiler, unsigned limit);
#ifdef __OBJC__
#define MTL_COMPILER_PIPELINE_CALL(compiler, expression) ({ \
   mtl_compiler *_kk_native_compiler = (mtl_compiler *)(compiler); \
   void *_kk_capture = mtl_compiler_cache_begin(_kk_native_compiler); \
   __typeof__(expression) _kk_pipeline = nil; \
   @try { \
      _kk_pipeline = (expression); \
      if (_kk_pipeline) mtl_compiler_cache_record_descriptor(_kk_native_compiler, NULL); \
   } @finally { mtl_compiler_cache_end(_kk_capture); } \
   _kk_pipeline; \
})
/* The expression creates the pipeline with the compiler named by native. */
#define MTL_COMPILER_PIPELINE_CALL_DESCRIPTOR(compiler, descriptor, native, expression) ({ \
   mtl_compiler *_kk_native_compiler = (mtl_compiler *)(compiler); \
   mtl_compiler *_kk_selected = _kk_native_compiler; \
   void *_kk_capture = mtl_compiler_cache_begin_pipeline(_kk_native_compiler, &_kk_selected); \
   id<MTL4Compiler> native = (id<MTL4Compiler>)_kk_selected; \
   __typeof__(expression) _kk_pipeline = nil; \
   @try { \
      _kk_pipeline = (expression); \
      if (_kk_pipeline) \
         mtl_compiler_cache_record_pipeline_descriptor(_kk_native_compiler, _kk_capture, \
                                                       (void *)(descriptor)); \
   } @finally { mtl_compiler_cache_end(_kk_capture); } \
   _kk_pipeline; \
})
#endif

/* Native compute jobs use the same compiler cache and a bounded, lazy queue.
 * Caller owns the task. Cleanup retires its C payload after synchronous Metal
 * work finishes, or immediately when a queued task is cancelled. */
void *mtl_compiler_submit(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *));
void mtl_compiler_task_wait(void *task);
void mtl_compiler_task_cancel(void *task);
/* How a wait completed a task: no wait, ran an unentered payload on the
 * waiting thread, waited for a queued or frontend task, or waited for a
 * deferred payload already compiling and raised its worker for the wait. */
enum mtl_compiler_join {
   MTL_COMPILER_JOIN_NONE,
   MTL_COMPILER_JOIN_HERE,
   MTL_COMPILER_JOIN_QUEUED,
   MTL_COMPILER_JOIN_RUNNING,
};
enum mtl_compiler_join mtl_compiler_task_join(void *task);
/* Raise the worker of a deferred payload already compiling until the
 * returned token is ended. NULL when no worker is compiling it. */
void *mtl_compiler_task_raise(void *task);
void mtl_compiler_task_raise_end(void *raised);
void *mtl_compiler_foreground_begin(mtl_compiler *compiler);
void mtl_compiler_foreground_end(void *token);
void mtl_compiler_store_shader(mtl_compiler *compiler, const void *key,
   size_t key_size, const void *bytes, size_t size);

/* Library */
mtl_library *
mtl_new_library(mtl_compiler *compiler, const char *src,
                enum mtl_language_version msl_version,
                enum mtl_math_mode math_mode,
                enum mtl_math_floating_point_functions math_fp_fns);
/* Used only on the bounded native compute workers. */
mtl_library *mtl_new_library_async_wait(mtl_compiler *compiler, const char *src,
   enum mtl_language_version version, enum mtl_math_mode mode,
   enum mtl_math_floating_point_functions fp);
/* Each source preserves its own language version. Both returned libraries are
 * owned by the caller; on failure both outputs are NULL. */
struct mtl_library_source {
   const char *code;
   enum mtl_language_version language_version;
};

bool mtl_new_library_pair(mtl_compiler *compiler,
                         const struct mtl_library_source sources[2],
                         enum mtl_math_mode math_mode,
                         enum mtl_math_floating_point_functions math_fp_fns,
                         mtl_library *libraries[2]);

mtl_function_descriptor *
mtl_new_library_function_descriptor(mtl_library *library,
                                    const char *entry_point);

mtl_function_descriptor *
mtl_specialize_function_uint64(mtl_function_descriptor *function,
                               uint32_t index, uint64_t value);

mtl_function_descriptor *mtl_specialize_pipeline_statistics(
   mtl_function_descriptor *function);
mtl_render_pipeline_descriptor *mtl_statistics_render_descriptor(
   mtl_render_pipeline_descriptor *descriptor);

/* Constant values wrap the complete caller chain; retained descriptors remain
 * reusable for both independent sampler domains. Stage bits occupy 0..3
 * for generic emulation and 4..7 for non-seamless cube sampling. */
enum mtl_sampler_emulation_stage {
   MTL_SAMPLER_EMULATION_VERTEX = 1u << 0,
   MTL_SAMPLER_EMULATION_FRAGMENT = 1u << 1,
   MTL_SAMPLER_EMULATION_OBJECT = 1u << 2,
   MTL_SAMPLER_EMULATION_MESH = 1u << 3,
};
mtl_function_descriptor *mtl_specialize_sampler_emulation(
   mtl_function_descriptor *function, uint32_t enabled);
mtl_render_pipeline_descriptor *mtl_sampler_emulation_render_descriptor(
   mtl_render_pipeline_descriptor *descriptor, uint32_t stages,
   uint32_t enabled_stages);
/* Copies a render or Mesh pipeline descriptor with a new fragment function,
 * keeping the archive key's record of the other stages' constant values. */
mtl_render_pipeline_descriptor *mtl_copy_render_pipeline_descriptor_with_fragment(
   mtl_render_pipeline_descriptor *descriptor, mtl_function_descriptor *fragment);

/* Compute pipeline */
mtl_compute_pipeline_state *
mtl_new_compute_pipeline_state(mtl_compiler *compiler,
                               mtl_function_descriptor *function,
                               uint64_t max_total_threads_per_threadgroup);

/* Render pipeline descriptor */
mtl_render_pipeline_descriptor *mtl_new_render_pipeline_descriptor(void);

void mtl_render_pipeline_descriptor_set_vertex_shader(
   mtl_render_pipeline_descriptor *descriptor, mtl_function_descriptor *shader);

void mtl_render_pipeline_descriptor_set_fragment_shader(
   mtl_render_pipeline_descriptor *descriptor, mtl_function_descriptor *shader);

void mtl_render_pipeline_descriptor_set_input_primitive_topology(
   mtl_render_pipeline_descriptor *descriptor,
   enum mtl_primitive_topology_class topology_class);

void mtl_render_pipeline_descriptor_set_color_attachment_format(
   mtl_render_pipeline_descriptor *descriptor, uint8_t index,
   enum mtl_pixel_format format);

void mtl_render_pipeline_descriptor_set_raster_sample_count(
   mtl_render_pipeline_descriptor *descriptor, uint32_t sample_count);

void mtl_render_pipeline_descriptor_set_rasterization_enabled(
   mtl_render_pipeline_descriptor *descriptor, bool enabled);

void mtl_render_pipeline_descriptor_set_max_vertex_amplification_count(
   mtl_render_pipeline_descriptor *descriptor, uint32_t count);

/* Render pipeline */
mtl_render_pipeline_state *
mtl_new_render_pipeline(mtl_compiler *compiler,
                        mtl_render_pass_descriptor *descriptor);

mtl_render_pipeline_state *mtl_new_render_pipeline_for_attachments(
   mtl_compiler *compiler, mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,
   uint32_t samples, bool framebuffer_reads);

mtl_render_pipeline_state *mtl_new_render_pipeline_for_raster_attachments(
   mtl_compiler *compiler, mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,
   uint32_t samples, bool framebuffer_reads, enum mtl_primitive_topology_class topology);

void mtl_render_pipeline_descriptor_copy_linking(
   mtl_render_pipeline_descriptor *copy, mtl_render_pipeline_descriptor *source);

#endif /* MTL_COMPILER_H */
