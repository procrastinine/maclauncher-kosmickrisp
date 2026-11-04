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
   void *(*load)(void *data, size_t *size);
   void (*store)(void *data, const void *bytes, size_t size);
   void (*destroy)(void *data);
};
mtl_compiler *mtl_new_cached_compiler(mtl_device *device,
   const struct mtl_compiler_cache_ops *ops, void *data);
struct mtl_compiler_cache_stats {
   uint64_t lookups, hits, native_compiles, stored_bytes;
   bool archive_loaded;
};
/* Lifetime totals for this shared native compiler, not one Vulkan device. */
bool mtl_compiler_get_cache_stats(mtl_compiler *compiler,
   struct mtl_compiler_cache_stats *stats);
/* Call only after the final compiler client has drained its queues. */
void mtl_compiler_finish_cache(mtl_compiler *compiler);

/* Library */
mtl_library *
mtl_new_library(mtl_compiler *compiler, const char *src,
                enum mtl_language_version msl_version,
                enum mtl_math_mode math_mode,
                enum mtl_math_floating_point_functions math_fp_fns);
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
