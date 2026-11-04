/* SPDX-License-Identifier: MIT */
#include "kk_interlock_shader_data.h"
#include "kk_device.h"
#include "kk_cmd_buffer.h"
#include "kk_depth_bounds.h"
#include "kk_atomic64.h"
#include "kk_physical_device.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "util/ralloc.h"

struct kk_interlock_pipeline_entry {
   struct kk_interlock_pipeline_entry *next;
   enum kk_interlock_program program;
   enum mtl_pixel_format formats[8];
   unsigned samples;
   enum mtl_primitive_topology_class topology;
   bool pipeline_statistics;
   bool retained_source;
   mtl_render_pipeline_state *handle;
};

struct kk_depth_bounds_pipeline_entry {
   struct kk_interlock_pipeline_entry base;
   mtl_render_pipeline_descriptor *source_descriptor;
};

void
kk_interlock_pipeline_finish(struct kk_interlock_pipeline *pipeline)
{
   if (!pipeline)
      return;
   for (struct kk_interlock_pipeline_entry *p = pipeline->entries; p; p = p->next) {
      mtl_release(p->handle);
      if (p->retained_source)
         mtl_release(container_of(p, struct kk_depth_bounds_pipeline_entry, base)->source_descriptor);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(pipeline->descriptor); ++i)
      mtl_release(pipeline->descriptor[i]);
   mtl_release(pipeline->load);
   if (pipeline->depth_bounds) {
      if (pipeline->bounds) {
         mtl_release(pipeline->bounds->clear);
         mtl_release(pipeline->bounds->capture_fragment);
      }
   } else {
      mtl_release(pipeline->replace);
   }
   simple_mtx_destroy(&pipeline->mutex);
   ralloc_free(pipeline);
}

/* Native attachment/sample state and an active fragment statistics query
 * specialize the already compiled source, just as in the ordinary cache. */
static VkResult
kk_interlock_pipeline_for_source(struct kk_device *dev,
   struct kk_interlock_pipeline *pipeline, enum kk_interlock_program program,
   const enum mtl_pixel_format formats[8], unsigned samples,
   enum mtl_primitive_topology_class topology, bool pipeline_statistics,
   mtl_render_pipeline_state **out,
   mtl_render_pipeline_descriptor *source_descriptor,
   mtl_render_pipeline_descriptor *capture_descriptor)
{
   assert(program <= KK_INTERLOCK_REPLAY);
   pipeline_statistics &= pipeline->uses_pipeline_statistics[program];
   simple_mtx_lock(&pipeline->mutex);
   for (struct kk_interlock_pipeline_entry *p = pipeline->entries; p; p = p->next) {
      if (p->program == program && p->samples == samples &&
          p->topology == topology &&
          p->pipeline_statistics == pipeline_statistics &&
          !memcmp(p->formats, formats, sizeof(p->formats)) &&
          p->retained_source == (source_descriptor != NULL) &&
          (!source_descriptor || container_of(p, struct kk_depth_bounds_pipeline_entry, base)->source_descriptor == source_descriptor)) {
         *out = p->handle;
         simple_mtx_unlock(&pipeline->mutex);
         return VK_SUCCESS;
      }
   }
   struct kk_interlock_pipeline_entry *p = source_descriptor ?
      (struct kk_interlock_pipeline_entry *)rzalloc(pipeline, struct kk_depth_bounds_pipeline_entry) :
      rzalloc(pipeline, struct kk_interlock_pipeline_entry);
   if (!p) {
      simple_mtx_unlock(&pipeline->mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   mtl_render_pipeline_descriptor *base = capture_descriptor ?
      capture_descriptor : pipeline->descriptor[program];
   mtl_render_pipeline_descriptor *descriptor = pipeline_statistics ?
      mtl_statistics_render_descriptor(base) : base;
   if (!descriptor) {
      ralloc_free(p);
      simple_mtx_unlock(&pipeline->mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   if (!source_descriptor)
      mtl_render_pipeline_descriptor_set_input_primitive_topology(descriptor, topology);
   p->handle = mtl_new_render_pipeline_for_attachments(dev->mtl_compiler_handle,
      descriptor, formats, 8, source_descriptor ? 0xffu : 0, samples, pipeline->framebuffer_reads[program]);
   if (pipeline_statistics)
      mtl_release(descriptor);
   if (!p->handle) {
      ralloc_free(p);
      simple_mtx_unlock(&pipeline->mutex);
      return VK_ERROR_INVALID_SHADER_NV;
   }
   p->retained_source = source_descriptor != NULL;
   if (source_descriptor)
      container_of(p, struct kk_depth_bounds_pipeline_entry, base)->source_descriptor = mtl_retain(source_descriptor);
   p->program = program;
   p->samples = samples;
   p->topology = topology;
   p->pipeline_statistics = pipeline_statistics;
   memcpy(p->formats, formats, sizeof(p->formats));
   p->next = pipeline->entries;
   pipeline->entries = p;
   *out = p->handle;
   simple_mtx_unlock(&pipeline->mutex);
   return VK_SUCCESS;
}

VkResult
kk_interlock_pipeline_get(struct kk_device *dev,
   struct kk_interlock_pipeline *pipeline, enum kk_interlock_program program,
   const enum mtl_pixel_format formats[8], unsigned samples,
   enum mtl_primitive_topology_class topology, bool pipeline_statistics,
   mtl_render_pipeline_state **out)
{
   return kk_interlock_pipeline_for_source(dev, pipeline, program, formats,
      samples, topology, pipeline_statistics, out, NULL, NULL);
}

static VkResult
kk_interlock_function(struct kk_device *dev, const struct msl_compile_data *source,
   mtl_function_descriptor **out)
{
   *out = NULL;
   if (!source->code)
      return VK_ERROR_INVALID_SHADER_NV;
   struct kk_physical_device *pdev = kk_device_physical(dev);
   mtl_library *library = mtl_new_library(dev->mtl_compiler_handle, source->code,
      kk_msl_language_version(source, pdev->info.msl_version),
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   if (!library)
      return VK_ERROR_INVALID_SHADER_NV;
   VkResult result = kk_msl_function(dev, source, library,
      source->entrypoint_name, out);
   mtl_release(library);
   return result;
}

static VkResult
kk_interlock_descriptor(struct kk_device *dev,
   mtl_function_descriptor *vertex, const struct msl_compile_data *fs,
   mtl_render_pipeline_descriptor **out)
{
   mtl_function_descriptor *fragment = NULL;
   VkResult result = kk_interlock_function(dev, fs, &fragment);
   if (result != VK_SUCCESS)
      return result;
   mtl_render_pipeline_descriptor *desc = mtl_new_render_pipeline_descriptor();
   mtl_render_pipeline_descriptor_set_vertex_shader(desc, vertex);
   mtl_render_pipeline_descriptor_set_fragment_shader(desc, fragment);
   mtl_render_pipeline_descriptor_set_rasterization_enabled(desc, true);
   mtl_release(fragment);
   *out = desc;
   return VK_SUCCESS;
}

VkResult
kk_interlock_pipeline_compile(struct kk_device *dev, struct kk_shader *shader)
{
   if (!shader->interlock)
      return VK_SUCCESS;
   struct kk_interlock_pipeline *pipeline = rzalloc(NULL, struct kk_interlock_pipeline);
   if (!pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   simple_mtx_init(&pipeline->mutex, mtx_plain);
   shader->interlock_pipeline = pipeline;
   pipeline->depth_bounds = shader->info.depth_bounds_emulation;
   if (pipeline->depth_bounds) {
      pipeline->bounds = rzalloc(pipeline, struct kk_depth_bounds_pipeline);
      if (!pipeline->bounds)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   const struct kk_interlock_shader *source = shader->interlock;
   memcpy(pipeline->framebuffer_reads, source->info.framebuffer_reads,
      sizeof(pipeline->framebuffer_reads));
   const struct msl_compile_data *load = &source->program[KK_INTERLOCK_LOAD];
   struct kk_physical_device *pdev = kk_device_physical(dev);
   mtl_library *library = mtl_new_library(dev->mtl_compiler_handle, load->code,
      kk_msl_language_version(load, pdev->info.msl_version),
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   if (!library)
      return VK_ERROR_INVALID_SHADER_NV;
   mtl_function_descriptor *function = NULL;
   VkResult result = kk_msl_function(dev, load, library,
      load->entrypoint_name, &function);
   if (result != VK_SUCCESS) {
      mtl_release(library);
      return result;
   }
   pipeline->load = mtl_new_compute_pipeline_state(dev->mtl_compiler_handle, function, 64);
   mtl_release(function);
   mtl_release(library);
   if (!pipeline->load)
      return VK_ERROR_INVALID_SHADER_NV;
   if (pipeline->depth_bounds) {
      const struct kk_depth_bounds_shader *bounds =
         container_of(source, const struct kk_depth_bounds_shader, base);
      if (bounds->clear.code) {
         function = NULL;
         result = kk_interlock_function(dev, &bounds->clear, &function);
         if (result != VK_SUCCESS)
            return result;
         pipeline->bounds->clear = mtl_new_compute_pipeline_state(dev->mtl_compiler_handle, function, 64);
         mtl_release(function);
         if (!pipeline->bounds->clear)
            return VK_ERROR_INVALID_SHADER_NV;
      }
   }
   const bool mesh = shader->info.stage == MESA_SHADER_MESH;
   mtl_function_descriptor *capture = NULL;
   result = mesh ? VK_SUCCESS : kk_interlock_function(dev, &shader->polygon[2], &capture);
   if (result != VK_SUCCESS)
      return result;
   /* Transaction and replay use exactly the same tile vertex program. */
   mtl_function_descriptor *tile = NULL;
   result = kk_interlock_function(dev,
      &source->program[KK_INTERLOCK_TILE_VERTEX], &tile);
   for (unsigned i = 0; result == VK_SUCCESS && i <= KK_INTERLOCK_REPLAY; ++i) {
      pipeline->uses_pipeline_statistics[i] =
         source->program[i].uses_pipeline_statistics;
      if (mesh && i == KK_INTERLOCK_CAPTURE)
         continue;
      result = kk_interlock_descriptor(dev,
         i == KK_INTERLOCK_CAPTURE ? capture : tile,
         &source->program[i], &pipeline->descriptor[i]);
   }
   mtl_release(capture);
   mtl_release(tile);
   if (result != VK_SUCCESS)
      return result;

   /* Replay replaces the software-tested values. Its state depends only on
    * this compiled envelope's depth attachment, never on dynamic app tests. */
   if (!pipeline->depth_bounds) {
      mtl_depth_stencil_descriptor *ds = mtl_new_depth_stencil_descriptor();
      mtl_depth_stencil_descriptor_set_depth_compare_function(ds, VK_COMPARE_OP_ALWAYS);
      mtl_depth_stencil_descriptor_set_depth_write_enabled(ds, source->info.depth);
      mtl_stencil_descriptor *st = mtl_new_stencil_descriptor();
      mtl_stencil_descriptor_set_stencil_compare_function(st, VK_COMPARE_OP_ALWAYS);
      mtl_stencil_descriptor_set_depth_stencil_pass_operation(st, VK_STENCIL_OP_REPLACE);
      mtl_stencil_descriptor_set_write_mask(st, 255);
      mtl_depth_stencil_descriptor_set_front_face_stencil(ds, st);
      mtl_depth_stencil_descriptor_set_back_face_stencil(ds, st);
      pipeline->replace = mtl_new_depth_stencil_state(dev->mtl_handle, ds);
      mtl_release(st);
      mtl_release(ds);
      if (!pipeline->replace)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   enum mtl_pixel_format empty[8] = {MTL_PIXEL_FORMAT_INVALID};
   for (unsigned i = 0; i < ARRAY_SIZE(empty); ++i)
      empty[i] = MTL_PIXEL_FORMAT_INVALID;
   unsigned samples = MAX2(shader->info.vs.sample_count, 1);
   mtl_render_pipeline_state *unused;
   result = mesh ? VK_SUCCESS : kk_interlock_pipeline_get(dev, pipeline, KK_INTERLOCK_CAPTURE,
      empty, samples, shader->info.vs.topology, false, &unused);
   if (result == VK_SUCCESS)
      result = kk_interlock_pipeline_get(dev, pipeline, KK_INTERLOCK_TRANSACTION,
         empty, 1, MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE, false, &unused);
   if (result == VK_SUCCESS)
      result = kk_interlock_pipeline_get(dev, pipeline, KK_INTERLOCK_REPLAY,
         shader->info.vs.rt_formats, samples, MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE,
         false, &unused);
   return result;
}

#include "kk_depth_bounds_mesh_pipeline.h"
