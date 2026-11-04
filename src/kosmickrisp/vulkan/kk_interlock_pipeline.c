/* SPDX-License-Identifier: MIT */
#include "kk_interlock_shader_data.h"
#include "kk_device.h"
#include "kk_physical_device.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "util/ralloc.h"

struct kk_interlock_pipeline_entry {
   struct kk_interlock_pipeline_entry *next;
   enum kk_interlock_program program;
   enum mtl_pixel_format formats[8];
   unsigned samples;
   enum mtl_primitive_topology_class topology;
   mtl_render_pipeline_state *handle;
};

void
kk_interlock_pipeline_finish(struct kk_interlock_pipeline *pipeline)
{
   if (!pipeline)
      return;
   for (struct kk_interlock_pipeline_entry *p = pipeline->entries; p; p = p->next)
      mtl_release(p->handle);
   for (unsigned i = 0; i < ARRAY_SIZE(pipeline->descriptor); ++i)
      mtl_release(pipeline->descriptor[i]);
   mtl_release(pipeline->load);
   mtl_release(pipeline->replace);
   simple_mtx_destroy(&pipeline->mutex);
   ralloc_free(pipeline);
}

/* Only native attachment/sample state varies here. All NIR and Metal program
 * sources were compiled eagerly; this is the same native descriptor
 * specialization used by the ordinary attachment pipeline cache. */
VkResult
kk_interlock_pipeline_get(struct kk_device *dev,
   struct kk_interlock_pipeline *pipeline, enum kk_interlock_program program,
   const enum mtl_pixel_format formats[8], unsigned samples,
   enum mtl_primitive_topology_class topology, mtl_render_pipeline_state **out)
{
   assert(program <= KK_INTERLOCK_REPLAY);
   simple_mtx_lock(&pipeline->mutex);
   for (struct kk_interlock_pipeline_entry *p = pipeline->entries; p; p = p->next) {
      if (p->program == program && p->samples == samples &&
          p->topology == topology && !memcmp(p->formats, formats, sizeof(p->formats))) {
         *out = p->handle;
         simple_mtx_unlock(&pipeline->mutex);
         return VK_SUCCESS;
      }
   }
   struct kk_interlock_pipeline_entry *p = rzalloc(pipeline,
      struct kk_interlock_pipeline_entry);
   if (!p) {
      simple_mtx_unlock(&pipeline->mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   mtl_render_pipeline_descriptor *descriptor =
      pipeline->descriptor[program];
   mtl_render_pipeline_descriptor_set_input_primitive_topology(descriptor, topology);
   p->handle = mtl_new_render_pipeline_for_attachments(dev->mtl_compiler_handle,
      descriptor, formats, 8, 0, samples, pipeline->framebuffer_reads[program]);
   if (!p->handle) {
      ralloc_free(p);
      simple_mtx_unlock(&pipeline->mutex);
      return VK_ERROR_INVALID_SHADER_NV;
   }
   p->program = program;
   p->samples = samples;
   p->topology = topology;
   memcpy(p->formats, formats, sizeof(p->formats));
   p->next = pipeline->entries;
   pipeline->entries = p;
   *out = p->handle;
   simple_mtx_unlock(&pipeline->mutex);
   return VK_SUCCESS;
}

static mtl_function_descriptor *
kk_interlock_function(struct kk_device *dev, const struct msl_compile_data *source)
{
   if (!source->code)
      return NULL;
   struct kk_physical_device *pdev = kk_device_physical(dev);
   mtl_library *library = mtl_new_library(dev->mtl_compiler_handle, source->code,
      kk_msl_language_version(source, pdev->info.msl_version),
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   if (!library)
      return NULL;
   mtl_function_descriptor *function =
      mtl_new_library_function_descriptor(library, source->entrypoint_name);
   mtl_release(library);
   return function;
}

static VkResult
kk_interlock_descriptor(struct kk_device *dev,
   mtl_function_descriptor *vertex, const struct msl_compile_data *fs,
   mtl_render_pipeline_descriptor **out)
{
   mtl_function_descriptor *fragment = kk_interlock_function(dev, fs);
   if (!fragment)
      return VK_ERROR_INVALID_SHADER_NV;
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
   mtl_function_descriptor *function =
      mtl_new_library_function_descriptor(library, load->entrypoint_name);
   pipeline->load = mtl_new_compute_pipeline_state(dev->mtl_compiler_handle, function, 64);
   mtl_release(function);
   mtl_release(library);
   if (!pipeline->load)
      return VK_ERROR_INVALID_SHADER_NV;
   mtl_function_descriptor *capture = kk_interlock_function(dev, &shader->polygon[2]);
   if (!capture)
      return VK_ERROR_INVALID_SHADER_NV;
   /* Transaction and replay use exactly the same tile vertex program. */
   mtl_function_descriptor *tile =
      kk_interlock_function(dev, &source->program[KK_INTERLOCK_TILE_VERTEX]);
   VkResult result = tile ? VK_SUCCESS : VK_ERROR_INVALID_SHADER_NV;
   for (unsigned i = 0; result == VK_SUCCESS && i <= KK_INTERLOCK_REPLAY; ++i) {
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
   enum mtl_pixel_format empty[8] = {MTL_PIXEL_FORMAT_INVALID};
   for (unsigned i = 0; i < ARRAY_SIZE(empty); ++i)
      empty[i] = MTL_PIXEL_FORMAT_INVALID;
   unsigned samples = MAX2(shader->info.vs.sample_count, 1);
   mtl_render_pipeline_state *unused;
   result = kk_interlock_pipeline_get(dev, pipeline, KK_INTERLOCK_CAPTURE,
      empty, samples, shader->info.vs.topology, &unused);
   if (result == VK_SUCCESS)
      result = kk_interlock_pipeline_get(dev, pipeline, KK_INTERLOCK_TRANSACTION,
         empty, 1, MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE, &unused);
   if (result == VK_SUCCESS)
      result = kk_interlock_pipeline_get(dev, pipeline, KK_INTERLOCK_REPLAY,
         shader->info.vs.rt_formats, samples, MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE, &unused);
   return result;
}
