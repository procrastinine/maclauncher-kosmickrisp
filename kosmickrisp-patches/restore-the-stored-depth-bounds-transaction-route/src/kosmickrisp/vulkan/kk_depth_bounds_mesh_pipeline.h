/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BOUNDS_MESH_PIPELINE_H
#define KK_DEPTH_BOUNDS_MESH_PIPELINE_H
#include "kk_depth_stencil_state.h"
#include "kk_format.h"
#include "kosmickrisp/libkk/kk_argument_table.h"

VkResult
kk_depth_bounds_mesh_capture_pipeline(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, mtl_render_pipeline_state **out)
{
   if (!cmd || !batch || !out || batch->producer != cmd->state.gfx.shader)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const struct kk_shader *producer = batch->producer;
   struct kk_interlock_pipeline *pipeline = producer->interlock_pipeline;
   if (!pipeline || !producer->interlock || !batch->raster_descriptor)
      return VK_ERROR_INITIALIZATION_FAILED;
   simple_mtx_lock(&pipeline->mutex);
   if (!pipeline->bounds->capture_fragment) {
      const struct msl_compile_data *capture =
         &producer->interlock->program[KK_INTERLOCK_CAPTURE];
      VkResult result = kk_interlock_function(dev, capture,
         &pipeline->bounds->capture_fragment);
      if (result != VK_SUCCESS) {
         simple_mtx_unlock(&pipeline->mutex);
         return result;
      }
   }
   mtl_function_descriptor *fragment = pipeline->bounds->capture_fragment;
   simple_mtx_unlock(&pipeline->mutex);
   if (!fragment)
      return VK_ERROR_INVALID_SHADER_NV;
   mtl_render_pipeline_descriptor *capture =
      kk_mesh_retained_fragment_descriptor(batch, fragment);
   if (!capture)
      return VK_ERROR_INITIALIZATION_FAILED;
   enum mtl_pixel_format formats[8];
   for (unsigned i = 0; i < ARRAY_SIZE(formats); ++i)
      formats[i] = MTL_PIXEL_FORMAT_INVALID;
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      unsigned logical = render->color_map[i];
      if (logical != MESA_VK_ATTACHMENT_UNUSED && render->color_att[i].iview)
         formats[logical] = vk_format_to_mtl_pixel_format(render->color_att[i].vk_format);
   }
   VkResult result = kk_interlock_pipeline_for_source(dev, pipeline,
      KK_INTERLOCK_CAPTURE, formats,
      cmd->vk.dynamic_graphics_state.ms.rasterization_samples,
      producer->info.vs.topology, false, out, batch->raster_descriptor, capture);
   mtl_release(capture);
   return result;
}

VkResult
kk_depth_bounds_mesh_capture_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch,
   const struct kk_depth_stencil_snapshot *snapshot, uint64_t env, uint64_t job,
   mtl_argument_table *fragment_table)
{
   if (!batch || !snapshot || snapshot->command != cmd || snapshot->producer != batch->producer ||
       snapshot->encoder != batch->encoder || !env || !job || !fragment_table)
      return VK_ERROR_INITIALIZATION_FAILED;
   const struct kk_depth_bounds_capture_context context = {
      .env = env, .original_per_draw = batch->per_draw_address, .job = job,
      .max_primitives = batch->layout.max_primitives,
   };
   struct kk_ptr captured = kk_pool_upload(cmd, &context, sizeof(context), 8);
   if (!captured.gpu)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   mtl_set_address(fragment_table, captured.gpu, KK_ARGUMENT_PER_DRAW);
   mtl_render_pipeline_state *pipeline;
   VkResult result = kk_depth_bounds_mesh_capture_pipeline(cmd, batch, &pipeline);
   if (result != VK_SUCCESS)
      return result;
   uint64_t parameters;
   result = kk_mesh_retained_raster_job_parameters(cmd, batch, job, &parameters);
   if (result != VK_SUCCESS)
      return result;
   result = kk_cmd_bind_depth_stencil(snapshot, KK_DEPTH_STENCIL_BYPASS);
   if (result != VK_SUCCESS)
      return result;
   /* Every use of the reusable Mesh arena is encoded inside its callback.
    * Binding ORIGINAL again performs no test or DS update by itself. */
   result = kk_mesh_retained_raster_replay_job(cmd, batch, parameters,
      pipeline, fragment_table);
   VkResult restored = kk_cmd_bind_depth_stencil(snapshot, KK_DEPTH_STENCIL_ORIGINAL);
   return result == VK_SUCCESS ? restored : result;
}
#endif
