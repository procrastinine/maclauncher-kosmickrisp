/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_compiler.h"

#include "mtl_format.h"

/* Compiler */
void mtl_init_debug_msl_salt(const char *salt) {}
mtl_compiler *
mtl_new_compiler(mtl_device *device)
{
   return NULL;
}

mtl_compiler *
mtl_new_cached_compiler(mtl_device *device,
   const struct mtl_compiler_cache_ops *ops, void *data)
{
   ops->destroy(data);
   return mtl_new_compiler(device);
}

bool mtl_compiler_get_cache_stats(mtl_compiler *compiler,
   struct mtl_compiler_cache_stats *stats) { return false; }

void mtl_compiler_finish_cache(mtl_compiler *compiler) {}

/* Library */
mtl_library *
mtl_new_library(mtl_compiler *compiler, const char *src,
                enum mtl_language_version msl_version,
                enum mtl_math_mode math_mode,
                enum mtl_math_floating_point_functions math_fp_fns)
{
   return NULL;
}

bool
mtl_new_library_pair(mtl_compiler *compiler,
                     const struct mtl_library_source sources[2],
                     enum mtl_math_mode math_mode,
                     enum mtl_math_floating_point_functions math_fp_fns,
                     mtl_library *libraries[2])
{
   libraries[0] = libraries[1] = NULL;
   return false;
}

mtl_function_descriptor *
mtl_new_library_function_descriptor(mtl_library *library,
                                    const char *entry_point)
{
   return NULL;
}

mtl_function_descriptor *
mtl_specialize_function_uint64(mtl_function_descriptor *function,
                                uint32_t index, uint64_t value)
{
   return NULL;
}

mtl_function_descriptor *
mtl_specialize_pipeline_statistics(mtl_function_descriptor *function)
{
   return NULL;
}

mtl_render_pipeline_descriptor *
mtl_statistics_render_descriptor(mtl_render_pipeline_descriptor *descriptor)
{
   return NULL;
}

mtl_function_descriptor *
mtl_specialize_sampler_emulation(mtl_function_descriptor *function, bool enabled)
{
   return NULL;
}

mtl_render_pipeline_descriptor *
mtl_sampler_emulation_render_descriptor(mtl_render_pipeline_descriptor *descriptor,
                                        uint32_t stages, uint32_t enabled_stages)
{
   return NULL;
}

/* Compute pipeline */

mtl_compute_pipeline_state *
mtl_new_compute_pipeline_state(mtl_compiler *compiler,
                               mtl_function_descriptor *function,
                               uint64_t max_total_threads_per_threadgroup)
{
   return NULL;
}

/* Render pipeline descriptor */
mtl_render_pipeline_descriptor *
mtl_new_render_pipeline_descriptor(void)
{
   return NULL;
}

void
mtl_render_pipeline_descriptor_set_vertex_shader(
   mtl_render_pipeline_descriptor *descriptor, mtl_function_descriptor *shader)
{
}

void
mtl_render_pipeline_descriptor_set_fragment_shader(
   mtl_render_pipeline_descriptor *descriptor, mtl_function_descriptor *shader)
{
}

void
mtl_render_pipeline_descriptor_set_input_primitive_topology(
   mtl_render_pipeline_descriptor *descriptor,
   enum mtl_primitive_topology_class topology_class)
{
}

void
mtl_render_pipeline_descriptor_set_color_attachment_format(
   mtl_render_pipeline_descriptor *descriptor, uint8_t index,
   enum mtl_pixel_format format)
{
}

void
mtl_render_pipeline_descriptor_set_raster_sample_count(
   mtl_render_pipeline_descriptor *descriptor, uint32_t sample_count)
{
}

void
mtl_render_pipeline_descriptor_set_rasterization_enabled(
   mtl_render_pipeline_descriptor *descriptor, bool enabled)
{
}

void
mtl_render_pipeline_descriptor_set_max_vertex_amplification_count(
   mtl_render_pipeline_descriptor *descriptor, uint32_t count)
{
}

/* Render pipeline */
mtl_render_pipeline_state *
mtl_new_render_pipeline(mtl_compiler *compiler,
                        mtl_render_pass_descriptor *descriptor)
{
   return NULL;
}

void
mtl_render_pipeline_descriptor_copy_linking(
   mtl_render_pipeline_descriptor *copy, mtl_render_pipeline_descriptor *source)
{
   /* Copies pipeline-linking state. */
}

mtl_render_pipeline_descriptor *
mtl_new_mesh_pipeline_descriptor(mtl_function_descriptor *mesh,
   mtl_function_descriptor *object, uint32_t mesh_threads,
   uint32_t object_threads, uint32_t payload_bytes, uint32_t mesh_groups)
{
   return NULL;
}

void *mtl_compiler_cache_begin(mtl_compiler *compiler) { return NULL; }
void mtl_compiler_cache_end(void *token) {}
void mtl_compiler_cache_record_pipeline(mtl_compiler *compiler) {}

void *mtl_compiler_submit(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *)) { return NULL; }
void mtl_compiler_task_wait(void *task) {}
void *mtl_compiler_foreground_begin(mtl_compiler *compiler) { return NULL; }
void mtl_compiler_foreground_end(void *token) {}
void mtl_compiler_store_shader(mtl_compiler *compiler, const void *key,
   size_t key_size, const void *bytes, size_t size) {}

mtl_library *mtl_new_library_async_wait(mtl_compiler *compiler, const char *src,
   enum mtl_language_version version, enum mtl_math_mode mode,
   enum mtl_math_floating_point_functions fp) { return NULL; }
