/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_cmd_buffer.h"
#include "kk_depth_stencil_state.h"
#include "kk_interlock_submit.h"
#include "kk_interlock_heap.h"
#include "kk_interlock_draw.h"
#include "util/half_float.h"
#include "kk-writable-msaa.h"
#include "kk_sparse.h"
#include "kk_dgc.h"

#include "kk_buffer.h"
#include "kk_cmd_pool.h"
#include "kk_descriptor_set_layout.h"
#include "kk_entrypoints.h"
#include "kk_format.h"
#include "kk_image_view.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_command_buffer.h"
#include "kosmickrisp/bridge/mtl_device.h"
#include "kosmickrisp/bridge/mtl_encoder.h"
#include "kosmickrisp/bridge/vk_to_mtl_map.h"

#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_cmd_queue.h"
#include "vk_format.h"
#include "vk_pipeline_layout.h"

static void
kk_descriptor_state_fini(struct kk_cmd_buffer *cmd,
                         struct kk_descriptor_state *desc)
{
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);

   for (unsigned i = 0; i < KK_MAX_SETS; i++) {
      vk_free(&pool->vk.alloc, desc->push[i]);
      desc->push[i] = NULL;
   }
}

static void
kk_cmd_release_resources(struct kk_device *dev, struct kk_cmd_buffer *cmd)
{
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);

   /* Internal views must go before their command-owned backing images. */
   vk_meta_object_list_reset(&dev->vk, &cmd->vk.meta_objects);
   util_dynarray_foreach(&cmd->msrtss_images, struct kk_msrtss_image, image) {
      kk_DestroyImage(kk_device_to_handle(dev), image->image, NULL);
      kk_FreeMemory(kk_device_to_handle(dev), image->memory, NULL);
   }
   util_dynarray_clear(&cmd->msrtss_images);
   kk_dgc_release_commands(cmd);
   kk_interlock_segments_finish(cmd);
   kk_interlock_heap_finish(cmd);
   kk_cmd_depth_stencil_finish(cmd);
   kk_cmd_release_dynamic_ds_state(cmd);
   kk_descriptor_state_fini(cmd, &cmd->state.gfx.descriptors);
   kk_descriptor_state_fini(cmd, &cmd->state.cs.descriptors);

   kk_cmd_pool_free_bo_list(pool, &cmd->uploader.bos);

   /* Release all BOs used as descriptor buffers for submissions */
   util_dynarray_foreach(&cmd->large_bos, struct kk_bo *, bo) {
      kk_destroy_bo(dev, *bo);
   }
   util_dynarray_clear(&cmd->large_bos);
   /* Native parts retire with their allocator in kk_release_command_allocators. */
   cmd->mesh_batches_in_part = cmd->mesh_draws_in_part = 0;
   util_dynarray_clear(&cmd->post_render_writes);
   util_dynarray_clear(&cmd->ts_resolves);
   util_dynarray_clear(&cmd->ts_stage_map);
   util_dynarray_foreach(&cmd->submit_drawables, void *, drawable)
      mtl_release(*drawable);
   util_dynarray_clear(&cmd->submit_drawables);
}

/* Ending recording preserves the allocator backing executable commands. */
static void
end_recording(struct kk_cmd_buffer *cmd)
{
   if (cmd->vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING ||
       cmd->metal.cmd_buf == NULL)
      return;

   cs_end(cmd);
   mtl_end_command_buffer(cmd->metal.cmd_buf);
}

static void
kk_release_command_allocators(struct kk_cmd_buffer *cmd)
{
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);

   /* Native buffers must return their recorded storage before reset
    * makes the allocator heaps reusable. Do not cache recorded buffers
    * across that boundary with an outstanding native storage reference. */
   if (cmd->metal.cmd_buf) {
      mtl_release(cmd->metal.cmd_buf);
      cmd->metal.cmd_buf = NULL;
   }
   util_dynarray_foreach(&cmd->mesh_command_buffers, mtl_command_buffer *, part)
      mtl_release(*part);
   util_dynarray_clear(&cmd->mesh_command_buffers);

   /* Reset/destroy may discard the recording only after it is no longer
    * pending. Each allocator belongs exclusively to this Vulkan command
    * buffer, including every native part of a split mesh recording. */
   if (cmd->metal.allocator) {
      mtl_command_allocator_reset(cmd->metal.allocator);
      kk_cmd_pool_return_allocator(pool, cmd->metal.allocator);
      cmd->metal.allocator = NULL;
   }
   util_dynarray_foreach(&cmd->mesh_allocators, mtl_command_allocator *, allocator) {
      mtl_command_allocator_reset(*allocator);
      kk_cmd_pool_return_allocator(pool, *allocator);
   }
   util_dynarray_clear(&cmd->mesh_allocators);
}

static void
kk_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct kk_cmd_buffer *cmd =
      container_of(vk_cmd_buffer, struct kk_cmd_buffer, vk);
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);

   if (cmd->drawable)
      mtl_release(cmd->drawable);

   /* Ensure closed command buffer for safe returns to pool */
   end_recording(cmd);
   kk_release_command_allocators(cmd);

   mtl_release(cmd->argument_table);
   mtl_release(cmd->mesh_table);

   vk_command_buffer_finish(&cmd->vk);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);

   kk_cmd_release_resources(dev, cmd);
   util_dynarray_fini(&cmd->large_bos);
   util_dynarray_fini(&cmd->dgc_commands);
   util_dynarray_fini(&cmd->mesh_command_buffers);
   util_dynarray_fini(&cmd->mesh_allocators);
   util_dynarray_fini(&cmd->post_render_writes);
   util_dynarray_fini(&cmd->msrtss_images);
   util_dynarray_fini(&cmd->ts_resolves);
   util_dynarray_fini(&cmd->ts_stage_map);
   util_dynarray_fini(&cmd->submit_drawables);

   vk_free(&pool->vk.alloc, cmd);
}

mtl_argument_table *
kk_new_command_argument_table(struct kk_device *dev)
{
   mtl_argument_table *table;
      mtl_argument_table_descriptor *desc = mtl_new_argument_table_descriptor();
      /* Root at 0, samplers at 1 and per draw data at 2 */
      mtl_set_max_buffer_binding_count(desc,
         kk_argument_table_buffer_count((struct kk_argument_table_usage){
            .interlock_guard = dev->interlock_guard != NULL,
            .poly_layer_replay = dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader || (dev->vk.enabled_features.multiview || dev->vk.enabled_features.pipelineFragmentShadingRate ||
          dev->vk.enabled_features.primitiveFragmentShadingRate || dev->vk.enabled_features.attachmentFragmentShadingRate),
         }));
      table = mtl_new_argument_table(dev->mtl_handle, desc);
      if (dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader || (dev->vk.enabled_features.multiview || dev->vk.enabled_features.pipelineFragmentShadingRate ||
          dev->vk.enabled_features.primitiveFragmentShadingRate || dev->vk.enabled_features.attachmentFragmentShadingRate))
         mtl_set_address(table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
      mtl_set_address(table, dev->samplers.table.bo->gpu, KK_ARGUMENT_SAMPLERS);
      if (dev->interlock_guard)
         mtl_set_address(table, dev->interlock_guard->gpu, KK_ARGUMENT_INTERLOCK_GUARD);
      mtl_release(desc);
   return table;
}

static VkResult
kk_create_cmd_buffer(struct vk_command_pool *vk_pool,
                     VkCommandBufferLevel level,
                     struct vk_command_buffer **cmd_buffer_out)
{
   struct kk_cmd_pool *pool = container_of(vk_pool, struct kk_cmd_pool, vk);
   struct kk_device *dev = kk_cmd_pool_device(pool);
   struct kk_cmd_buffer *cmd;
   VkResult result;

   cmd = vk_zalloc(&pool->vk.alloc, sizeof(*cmd), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = vk_command_buffer_init_with_params(
      &cmd->vk, &(struct vk_command_buffer_init_params){
                   .pool = &pool->vk,
                   .ops = &kk_cmd_buffer_ops,
                   .level = level,
                   .needs_cmd_queue = true,
                });
   if (result != VK_SUCCESS)
      goto alloc_fail;

   cmd->msrtss_images = UTIL_DYNARRAY_INIT;
   cmd->ts_resolves = UTIL_DYNARRAY_INIT;
   cmd->ts_stage_map = UTIL_DYNARRAY_INIT;
   cmd->submit_drawables = UTIL_DYNARRAY_INIT;
   cmd->mesh_command_buffers = UTIL_DYNARRAY_INIT;
   cmd->mesh_allocators = UTIL_DYNARRAY_INIT;
   cmd->post_render_writes = UTIL_DYNARRAY_INIT;

   cmd->argument_table = kk_new_command_argument_table(dev);

   cmd->large_bos = UTIL_DYNARRAY_INIT;
   cmd->dgc_commands = UTIL_DYNARRAY_INIT;

   cmd->vk.dynamic_graphics_state.vi = &cmd->state.gfx._dynamic_vi;
   cmd->vk.dynamic_graphics_state.ms.sample_locations =
      &cmd->state.gfx._dynamic_sl;

   list_inithead(&cmd->uploader.bos);

   *cmd_buffer_out = &cmd->vk;

   return VK_SUCCESS;

alloc_fail:
   vk_free(&pool->vk.alloc, cmd);
   return result;
}

static void
kk_reset_cmd_buffer_internal(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);

   /* Ensure closed command buffer so we can start it again */
   end_recording(cmd);
   kk_release_command_allocators(cmd);
   kk_cmd_release_resources(dev, cmd);

   cmd->uploader.bo = NULL;
   cmd->uploader.offset = 0;

   memset(&cmd->state, 0, sizeof(cmd->state));
   cmd->uses_heap = false;
   memset(&cmd->whole_pass, 0, sizeof(cmd->whole_pass));
   cmd->deferred_render_chain = false;
}

static void
kk_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer,
                    UNUSED VkCommandBufferResetFlags flags)
{
   struct kk_cmd_buffer *cmd =
      container_of(vk_cmd_buffer, struct kk_cmd_buffer, vk);

   /* Retirement already resets each replay buffer before returning it. */
   if (flags == 0 &&
       cmd->vk.state == MESA_VK_COMMAND_BUFFER_STATE_INITIAL)
      return;

   kk_reset_cmd_buffer_internal(cmd);
   vk_command_buffer_reset(&cmd->vk);
   cmd->submitted = false;
   cmd->one_time_submit = false;
   cmd->enqueue_commands = false;
}

const struct vk_command_buffer_ops kk_cmd_buffer_ops = {
   .create = kk_create_cmd_buffer,
   .reset = kk_reset_cmd_buffer,
   .destroy = kk_destroy_cmd_buffer,
};

VKAPI_ATTR VkResult VKAPI_CALL
kk_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                      const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);

   /* Implicit reset must discard old native storage before acquiring the
    * next recording, as explicit reset and destruction already do. */
   vk_command_buffer_begin(&cmd->vk, pBeginInfo);

   /* If this is the first time starting the command buffer allocate Metal
    * resources */
   if (cmd->metal.cmd_buf == NULL) {
      cmd->metal.cmd_buf = kk_cmd_pool_get_cmd_buf(pool);

      if (cmd->metal.cmd_buf == NULL)
         goto fail_cmd;

      if (cmd->vk.base.object_name)
         mtl_command_buffer_set_label(cmd->metal.cmd_buf,
                                      cmd->vk.base.object_name);
   }

   for (unsigned i = 0; i < 64; i++)
      cmd->state.cs.descriptors.root.cs.matrix_coordinates[i] = _mesa_float_to_half((float)i);
   cmd->enqueue_commands = cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
      kk_cmd_buffer_device(cmd)->vk.enabled_features.descriptorHeap;
   struct kk_device *sparse_dev = kk_cmd_buffer_device(cmd);
   cmd->sparse_guarded = kk_sparse_residency_used(sparse_dev);
   cmd->one_time_submit = !cmd->enqueue_commands &&
      (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) &&
      (!sparse_dev->vk.enabled_features.sparseResidencyBuffer || cmd->sparse_guarded);

   /* vk_command_buffer_begin will reset the command buffer meaning the
    * allocator may be returned to the pool. Request a new one. */
   if (cmd->metal.allocator == NULL) {
      cmd->metal.allocator = kk_cmd_pool_get_allocator(pool);

      if (cmd->metal.allocator == NULL)
         goto fail_allocator;
   }
   mtl_begin_command_buffer(cmd->metal.cmd_buf, cmd->metal.allocator);
   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && pBeginInfo->pInheritanceInfo) {
      const VkCommandBufferInheritanceDescriptorHeapInfoEXT *heap = vk_find_struct_const(
         pBeginInfo->pInheritanceInfo->pNext, COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT);
      if (heap) {
         if (heap->pSamplerHeapBindInfo)
            vk_enqueue_cmd_bind_sampler_heap_ext(&cmd->vk.cmd_queue, heap->pSamplerHeapBindInfo);
         if (heap->pResourceHeapBindInfo)
            vk_enqueue_cmd_bind_resource_heap_ext(&cmd->vk.cmd_queue, heap->pResourceHeapBindInfo);
      }
   }

   return VK_SUCCESS;

fail_allocator:
   mtl_release(cmd->metal.cmd_buf);
   cmd->metal.cmd_buf = NULL;
fail_cmd:
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY &&
       cmd->vk.record_result == VK_SUCCESS) {
      cmd->deferred_render_chain = kk_whole_pass_needs_submit(cmd);
      struct vk_command_buffer *source = &cmd->vk;
      cmd->transient_safe = kk_whole_pass_transient_safe(kk_cmd_buffer_device(cmd),
                                                         &source, 1);
      if (!cmd->enqueue_commands && !cmd->deferred_render_chain)
         mtl_execute_command_queue(&cmd->vk.cmd_queue, commandBuffer,
                              &kk_cmd_buffer_device(cmd)->vk.dispatch_table);
   }
   end_recording(cmd);

   return vk_command_buffer_end(&cmd->vk);
}

static void
kk_encoder_update_debug(struct kk_cmd_buffer *cmd, mtl_command_encoder *encoder)
{
   /* Since there are many Metal command buffers and encoders for each Vulkan
    * command buffer, we need to copy debug state from Vulkan to Metal when
    * new Metal objects are created. */
   if (cmd->vk.base.object_name)
      mtl_encoder_set_label(encoder, cmd->vk.base.object_name);

   util_dynarray_foreach(&cmd->vk.labels, VkDebugUtilsLabelEXT, label)
      mtl_encoder_push_debug_group(encoder, label->pLabelName);
}

void
cs_start_render(struct kk_cmd_buffer *cmd)
{
   struct kk_graphics_state *state = &cmd->state.gfx;
   uint32_t view_mask = state->render.view_mask;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   if (view_mask && vs && (vs->info.api_geometry ||
       state->layer_replay.bind ||
       (vs->info.poly_layer_replay && state->polygon_points) ||
       cmd->state.shaders[MESA_SHADER_TESS_EVAL]))
      view_mask = 0; /* A Poly replay writes its explicit layer. */
   assert(state->render_pass_descriptor);

   /* Ensure no other active encoder, aka compute */
   cs_end(cmd);
   kk_writable_msaa_prepare_render(cmd);

   cmd->metal.render = mtl_new_render_command_encoder_with_descriptor(
      cmd->metal.cmd_buf, state->render_pass_descriptor);

   kk_encoder_update_debug(cmd, cmd->metal.render);
   /* Starting a new render pass means we already flushed and no barrier is
    * needed. */
   state->render.write_available = false;
   state->render.ds_write_available = false;
   state->render.storage_write_available = false;

   uint32_t layer_ids[KK_MAX_MULTIVIEW_VIEW_COUNT] = {};
   uint32_t count = 0u;
   u_foreach_bit(id, view_mask)
      layer_ids[count++] = id;
   if (view_mask == 0u) {
      layer_ids[count++] = 0;
   }
   mtl_set_vertex_amplification_count(cmd->metal.render, layer_ids, count);

   /* Argument table won't ever change */
   mtl_render_set_argument_table(
      cmd->metal.render, cmd->argument_table,
      MTL_RENDER_STAGE_VERTEX | MTL_RENDER_STAGE_FRAGMENT);

   kk_cmd_buffer_dirty_all_gfx(cmd);
}

mtl_render_encoder *
cs_get_render(struct kk_cmd_buffer *cmd)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;

   if (gfx->need_to_start_render_pass) {
      gfx->render.samples = gfx->pipeline_sample_count;
      mtl_render_pass_descriptor_set_default_raster_sample_count(
         cmd->state.gfx.render_pass_descriptor, gfx->render.samples);
      gfx->need_to_start_render_pass = false;
      cs_start_render(cmd);
   }

   return cmd->metal.render;
}

static mtl_compute_encoder *
cs_get_compute_internal(struct kk_cmd_buffer *cmd, bool preserve_pipeline)
{
   /* If we are inside render, we need to force a restart later */
   if (cmd->metal.render) {
      kk_apply_attachment_store_ops(cmd, true);
      cs_end(cmd);
      kk_cmd_buffer_dirty_all_gfx(cmd);
      cmd->state.gfx.need_to_start_render_pass = true;
   }

   if (cmd->metal.compute == NULL) {
      cmd->state.cs.bound_pipeline = NULL;
      cmd->metal.compute = mtl_new_compute_command_encoder(cmd->metal.cmd_buf);
      mtl_compute_set_argument_table(cmd->metal.compute, cmd->argument_table);
      kk_encoder_update_debug(cmd, cmd->metal.compute);
   }

   if (!preserve_pipeline)
      cmd->state.cs.bound_pipeline = NULL;
   return cmd->metal.compute;
}

mtl_compute_encoder *
cs_get_compute(struct kk_cmd_buffer *cmd)
{
   /* Internal compute work may bind a different pipeline directly. */
   return cs_get_compute_internal(cmd, false);
}

mtl_compute_encoder *
cs_get_compute_for_dispatch(struct kk_cmd_buffer *cmd)
{
   return cs_get_compute_internal(cmd, true);
}

static void
end_encoder(struct kk_cmd_buffer *cmd, mtl_command_encoder *encoder)
{
   /* TODO_KOSMICKRISP This is probably overkill */
   mtl_barrier_after_stages(encoder, MTL_STAGE_ALL, MTL_STAGE_ALL);
   mtl_end_encoding(encoder);
   mtl_release(encoder);

   /* Fold the pending timestamp counter-heap resolves into `cmd_buf` */
   util_dynarray_foreach(&cmd->ts_resolves, struct kk_ts_resolve, r) {
      mtl_command_resolve_counter_heap(cmd->metal.cmd_buf, r->heap, r->index,
                                       1u, r->dst_addr);
   }

   util_dynarray_clear(&cmd->ts_resolves);
}

static void
flush_post_render_writes(struct kk_cmd_buffer *cmd)
{
   assert(cmd->metal.render == NULL);

   if (!util_dynarray_num_elements(&cmd->post_render_writes,
                                   struct libkk_imm_write))
      return;

   util_dynarray_foreach(&cmd->post_render_writes, struct libkk_imm_write, w) {
      libkk_write_u32(cmd, kk_grid_1d(1), true, (uint64_t)w->address, w->value);
   }

   util_dynarray_clear(&cmd->post_render_writes);
}

void
cs_end(struct kk_cmd_buffer *cmd)
{
   assert(cmd);
   kk_whole_pass_flush_suspended(cmd);

   /* Render first as it may open a compute encoder for post render writes. */
   if (cmd->metal.render) {
      end_encoder(cmd, cmd->metal.render);
      cmd->metal.render = NULL;

      /* The stage map is only valid for the current render encoder */
      util_dynarray_clear(&cmd->ts_stage_map);
      flush_post_render_writes(cmd);
      kk_writable_msaa_publish_render(cmd);
   }

   if (cmd->metal.compute) {
      end_encoder(cmd, cmd->metal.compute);
      cmd->metal.compute = NULL;
   }
}

/* Keep long virtual mesh schedules in bounded native command buffers. All
 * parts retain the same Vulkan resources. Separate native allocators bound
 * Metal's command-storage resource index even when most slices execute zero
 * workgroups. Allocators remain with the executable recording until reset
 * or destruction; submission order and query availability are preserved. */
bool
kk_mesh_split_command_buffer(struct kk_cmd_buffer *cmd)
{
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);
   mtl_command_buffer *next = kk_cmd_pool_get_cmd_buf(pool);
   if (!next) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   mtl_command_allocator *allocator = kk_cmd_pool_get_allocator(pool);
   if (!allocator) {
      kk_cmd_pool_return_cmd_buf(pool, next);
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   if (cmd->metal.render)
      kk_apply_attachment_store_ops(cmd, true);
   cs_end(cmd);
   mtl_end_command_buffer(cmd->metal.cmd_buf);
   util_dynarray_append(&cmd->mesh_command_buffers, cmd->metal.cmd_buf);
   util_dynarray_append(&cmd->mesh_allocators, cmd->metal.allocator);
   cmd->metal.cmd_buf = next;
   cmd->metal.allocator = allocator;
   cmd->mesh_batches_in_part = cmd->mesh_draws_in_part = 0;
   mtl_begin_command_buffer(next, allocator);
   if (cmd->vk.base.object_name)
      mtl_command_buffer_set_label(next, cmd->vk.base.object_name);
   cmd->state.gfx.need_to_start_render_pass = true;
   kk_cmd_buffer_dirty_all_gfx(cmd);
   return true;
}

void
kk_cmd_bind_root_to_argument_table(struct kk_cmd_buffer *cmd, uint64_t addr)
{
   /* Internal precompiled dispatches restore root_addr before returning.
    * A detached interlock argument table still needs its own first bind. */
   if (cmd->state.root_table == cmd->argument_table &&
       cmd->state.root_addr == addr)
      return;
   mtl_set_address(cmd->argument_table, addr, KK_ARGUMENT_ROOT);
   cmd->state.root_addr = addr;
   cmd->state.root_table = cmd->argument_table;
}

/* Returns true if a render pass split is required. The main cases are:
 * - Texture write -> barrier -> attachment read
 * - Attachment write -> barrier -> texture read
 * - Depth/stencil write -> barrier -> depth/stencil read (Metal limitation)
 *
 * For color attachment write -> barrier -> color attachment read there is no
 * need to split the render pass.
 *
 * TODO_KOSMICKRISP Potential improvement would be to track where the
 * attachments lie in memory and check against the buffers/images provided in
 * the barrier to avoid splitting a render pass that will never overlap the
 * attachment memory. Meaning there was no write to the attachment and therefore
 * no split is needed.
 */
static bool
kk_barrier_requires_encoder_split(struct kk_cmd_buffer *cmd,
                                  const VkDependencyInfo *dep)
{
   const VkAccessFlags2 texture_read_access =
      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
   const VkAccessFlags2 any_write_access =
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
      VK_ACCESS_2_MEMORY_WRITE_BIT;
   const VkAccessFlags2 ds_write_access =
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_2_MEMORY_WRITE_BIT;
   const VkAccessFlags2 storage_write_access =
      VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
      VK_ACCESS_2_MEMORY_WRITE_BIT;
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   const bool has_ds =
      render->depth_att.iview != NULL || render->stencil_att.iview != NULL;

   VkAccessFlags2 src = 0, dst = 0;
   for (uint32_t i = 0; i < dep->memoryBarrierCount; i++) {
      src |= kk_whole_pass_attachment_access(dep->pMemoryBarriers[i].srcAccessMask, dep->pMemoryBarriers[i].srcStageMask);
      dst |= kk_whole_pass_attachment_access(dep->pMemoryBarriers[i].dstAccessMask, dep->pMemoryBarriers[i].dstStageMask);
   }
   for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; i++) {
      src |= kk_whole_pass_attachment_access(dep->pBufferMemoryBarriers[i].srcAccessMask, dep->pBufferMemoryBarriers[i].srcStageMask);
      dst |= kk_whole_pass_attachment_access(dep->pBufferMemoryBarriers[i].dstAccessMask, dep->pBufferMemoryBarriers[i].dstStageMask);
   }
   for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; i++) {
      src |= kk_whole_pass_attachment_access(dep->pImageMemoryBarriers[i].srcAccessMask, dep->pImageMemoryBarriers[i].srcStageMask);
      dst |= kk_whole_pass_attachment_access(dep->pImageMemoryBarriers[i].dstAccessMask, dep->pImageMemoryBarriers[i].dstStageMask);
   }

   const VkMemoryRangeBarriersInfoKHR *ranges = vk_find_struct_const(
      dep->pNext, MEMORY_RANGE_BARRIERS_INFO_KHR);
   for (uint32_t i = 0; ranges && i < ranges->memoryRangeBarrierCount; ++i) {
      src |= ranges->pMemoryRangeBarriers[i].srcAccessMask;
      dst |= ranges->pMemoryRangeBarriers[i].dstAccessMask;
   }

   if (src & any_write_access)
      render->write_available = true;
   if (has_ds && (src & ds_write_access))
      render->ds_write_available = true;
   if (src & storage_write_access)
      render->storage_write_available = true;

   if (render->write_available && (dst & texture_read_access))
      return true;

   return (render->ds_write_available || render->storage_write_available) &&
          (dst & VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                       const VkDependencyInfo *pDependencyInfo)
{
   /* Synchronization2 carries execution scopes in the barrier elements. An
    * entirely empty dependency needs neither an encoder barrier nor a render
    * pass restart. Keep extension chains and dependency flags on the existing
    * path, and preserve barriers with stage scopes but empty access masks. */
   if (pDependencyInfo->pNext == NULL &&
       pDependencyInfo->dependencyFlags == 0 &&
       pDependencyInfo->memoryBarrierCount == 0 &&
       pDependencyInfo->bufferMemoryBarrierCount == 0 &&
       pDependencyInfo->imageMemoryBarrierCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   /* TODO_KOSMICKRISP Lighten barriers according to the actual requested
    * barrier. To take advantage of this we need to remove the chaining of
    * encoders. */
   if (cmd->metal.render) {
      /* Framebuffer-local dependencies remain on tile. Texture visibility
       * still uses the shared encoder-split decision. */
      const bool local = kk_whole_pass_local_barrier(pDependencyInfo);
      if (kk_barrier_requires_encoder_split(cmd, pDependencyInfo) ||
          (cmd->state.gfx.render.samples > 1 && !local)) {
         kk_apply_attachment_store_ops(cmd, true);
         cs_end(cmd);
         cs_start_render(cmd);
      } else if (!kk_whole_pass_attachment_barrier(pDependencyInfo)) {
         bool mesh = cmd->state.gfx.shader &&
            cmd->state.gfx.shader->info.stage == MESA_SHADER_MESH;
         enum mtl_stages before = local ? MTL_STAGE_FRAGMENT : mesh
            ? MTL_STAGE_OBJECTS | MTL_STAGE_MESH | MTL_STAGE_FRAGMENT : MTL_STAGE_VERTEX;
         enum mtl_stages after = mesh && !local ? before : MTL_STAGE_FRAGMENT;
         mtl_barrier_after_encoder_stages(cmd->metal.render, before, after);
      }
   } else if (cmd->metal.compute) {
      /* We chain encoders, so an intra-encoder barrier is enough here:
       * no need to tear down and recreate the encoder.
       */
      mtl_barrier_after_encoder_stages(cmd->metal.compute,
                                       MTL_STAGE_DISPATCH | MTL_STAGE_BLIT,
                                       MTL_STAGE_DISPATCH | MTL_STAGE_BLIT);
   }
}

static void
kk_bind_descriptor_sets(struct kk_descriptor_state *desc,
                        const VkBindDescriptorSetsInfoKHR *info)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);

   /* A repeated ordinary bind without dynamic descriptors changes neither
    * root addresses nor layout-derived offset slots. Preserve pending dirty
    * state and push descriptors exactly as on the general path. */
   if (info->descriptorSetCount == 1 && info->dynamicOffsetCount == 0 &&
       pipeline_layout->set_layouts[info->firstSet] != NULL &&
       vk_to_kk_descriptor_set_layout(
          pipeline_layout->set_layouts[info->firstSet])->vk.dynamic_descriptor_count == 0) {
      VK_FROM_HANDLE(kk_descriptor_set, set, info->pDescriptorSets[0]);
      if (set != NULL && desc->sets[info->firstSet] == set)
         return;
   }


   /* From the Vulkan 1.3.275 spec:
    *
    *    "When binding a descriptor set (see Descriptor Set Binding) to
    *    set number N...
    *
    *    If, additionally, the previously bound descriptor set for set
    *    N was bound using a pipeline layout not compatible for set N,
    *    then all bindings in sets numbered greater than N are
    *    disturbed."
    *
    * This means that, if some earlier set gets bound in such a way that
    * it changes set_dynamic_buffer_start[s], this binding is implicitly
    * invalidated.
    */
   uint8_t dyn_buffer_start =
      pipeline_layout->dynamic_descriptor_offset[info->firstSet];

   bool root_changed = false;
   uint32_t next_dyn_offset = 0;
   for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
      unsigned s = i + info->firstSet;
      VK_FROM_HANDLE(kk_descriptor_set, set, info->pDescriptorSets[i]);

      if (desc->sets[s] != set) {
         const uint64_t addr = set ? set->addr : 0;
         const uint32_t size = set ? set->size : 0;
         root_changed |= desc->root.sets[s] != addr ||
                         desc->set_sizes[s] != size;
         if (set != NULL) {
            desc->root.sets[s] = set->addr;
            desc->set_sizes[s] = set->size;
         } else {
            desc->root.sets[s] = 0;
            desc->set_sizes[s] = 0;
         }
         desc->sets[s] = set;

         /* Binding descriptors invalidates push descriptors */
         desc->push_dirty &= ~BITFIELD_BIT(s);
      }

      if (pipeline_layout->set_layouts[s] != NULL) {
         const struct kk_descriptor_set_layout *set_layout =
            vk_to_kk_descriptor_set_layout(pipeline_layout->set_layouts[s]);

         if (set != NULL && set_layout->vk.dynamic_descriptor_count > 0) {
            for (uint32_t j = 0; j < set_layout->vk.dynamic_descriptor_count;
                 j++) {
               struct kk_buffer_address addr = set->dynamic_buffers[j];
               addr.base_addr += info->pDynamicOffsets[next_dyn_offset + j];
               struct kk_buffer_address *previous =
                  &desc->root.dynamic_buffers[dyn_buffer_start + j];
               if (previous->base_addr != addr.base_addr ||
                   previous->size != addr.size || previous->zero != addr.zero) {
                  *previous = addr;
                  root_changed = true;
               }
            }
            next_dyn_offset += set->layout->vk.dynamic_descriptor_count;
         }

         dyn_buffer_start += set_layout->vk.dynamic_descriptor_count;
      } else {
         assert(set == NULL);
      }
   }
   assert(dyn_buffer_start <= KK_MAX_DYNAMIC_BUFFERS);
   assert(next_dyn_offset <= info->dynamicOffsetCount);

   /* Dynamic offsets are compared in their layout-derived slots, so
    * incompatible layouts still update every affected root address. Preserve
    * dirty state from pushes, pipeline changes and earlier binds. */
   desc->root_dirty |= root_changed;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindDescriptorSets2KHR(
   VkCommandBuffer commandBuffer,
   const VkBindDescriptorSetsInfoKHR *pBindDescriptorSetsInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   if (pBindDescriptorSetsInfo->stageFlags & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) {
      kk_bind_descriptor_sets(&cmd->state.gfx.descriptors,
                              pBindDescriptorSetsInfo);
   }

   if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      kk_bind_descriptor_sets(&cmd->state.cs.descriptors,
                              pBindDescriptorSetsInfo);
   }
}

static struct kk_push_descriptor_set *
kk_cmd_push_descriptors(struct kk_cmd_buffer *cmd,
                        struct kk_descriptor_state *desc,
                        struct kk_descriptor_set_layout *set_layout,
                        uint32_t set)
{
   assert(set < KK_MAX_SETS);
   uint32_t required = set_layout->non_variable_descriptor_buffer_size;
   if (unlikely(desc->push[set] == NULL || desc->push[set]->capacity < required)) {
      uint32_t previous = desc->push[set] ? desc->push[set]->capacity : 0;
      size_t size = sizeof(*desc->push[set]) + required;
      struct kk_push_descriptor_set *push = vk_realloc(
         &cmd->vk.pool->alloc, desc->push[set], size, 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (unlikely(push == NULL)) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      memset(push->data + previous, 0, required - previous);
      push->capacity = required;
      desc->push[set] = push;
   }

   /* Pushing descriptors replaces whatever sets are bound */
   desc->push[set]->layout = set_layout;
   desc->sets[set] = NULL;
   desc->push_dirty |= BITFIELD_BIT(set);

   return desc->push[set];
}

static void
kk_push_descriptor_set(struct kk_cmd_buffer *cmd,
                       struct kk_descriptor_state *desc,
                       const VkPushDescriptorSetInfoKHR *info)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, info->layout);

   struct kk_descriptor_set_layout *set_layout =
      vk_to_kk_descriptor_set_layout(pipeline_layout->set_layouts[info->set]);

   struct kk_push_descriptor_set *push_set =
      kk_cmd_push_descriptors(cmd, desc, set_layout, info->set);
   if (unlikely(push_set == NULL))
      return;

   kk_push_descriptor_set_update(push_set, info->descriptorWriteCount,
                                 info->pDescriptorWrites);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdPushDescriptorSet2KHR(
   VkCommandBuffer commandBuffer,
   const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   if (pPushDescriptorSetInfo->stageFlags & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) {
      kk_push_descriptor_set(cmd, &cmd->state.gfx.descriptors,
                             pPushDescriptorSetInfo);
   }

   if (pPushDescriptorSetInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      kk_push_descriptor_set(cmd, &cmd->state.cs.descriptors,
                             pPushDescriptorSetInfo);
   }
}

static void
kk_push_constants(UNUSED struct kk_cmd_buffer *cmd,
                  struct kk_descriptor_state *desc,
                  const VkPushConstantsInfoKHR *info)
{
   kk_descriptor_push_update(desc, info->offset, info->pValues, info->size);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdPushConstants2KHR(VkCommandBuffer commandBuffer,
                        const VkPushConstantsInfoKHR *pPushConstantsInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   if (pPushConstantsInfo->stageFlags & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT))
      kk_push_constants(cmd, &cmd->state.gfx.descriptors, pPushConstantsInfo);

   if (pPushConstantsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
      kk_push_constants(cmd, &cmd->state.cs.descriptors, pPushConstantsInfo);
}

void
kk_cmd_release_dynamic_ds_state(struct kk_cmd_buffer *cmd)
{
   if (cmd->state.gfx.is_depth_stencil_dynamic &&
       cmd->state.gfx.depth_stencil_state)
      mtl_release(cmd->state.gfx.depth_stencil_state);
   cmd->state.gfx.depth_stencil_state = NULL;
}

static VkResult
kk_cmd_buffer_alloc_bo(struct kk_cmd_buffer *cmd, struct kk_cmd_bo **bo_out)
{
   VkResult result = kk_cmd_pool_alloc_bo(kk_cmd_buffer_pool(cmd), bo_out);
   if (result != VK_SUCCESS)
      return result;

   list_addtail(&(*bo_out)->link, &cmd->uploader.bos);
   return VK_SUCCESS;
}

struct kk_ptr
kk_pool_alloc(struct kk_cmd_buffer *cmd, uint32_t size_B, uint32_t alignment_B)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_uploader *uploader = &cmd->uploader;

   /* Specially handle large allocations owned by the command buffer, e.g. used
    * for statically allocated vertex output buffers with geometry shaders.
    */
   if (size_B > KK_CMD_BO_SIZE) {
      struct kk_bo *buffer = NULL;
      const VkResult result =
         kk_alloc_bo(dev, &cmd->vk.base, size_B, alignment_B, &buffer);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return (struct kk_ptr){0};
      }
      util_dynarray_append(&cmd->large_bos, buffer);

      return (struct kk_ptr){
         .gpu = buffer->gpu,
         .cpu = buffer->cpu,

         .buffer = buffer->map,
         .offset = 0u,
      };
   }

   assert(size_B <= KK_CMD_BO_SIZE);
   assert(alignment_B > 0);

   const uint32_t offset = align(uploader->offset, alignment_B);

   assert(offset <= KK_CMD_BO_SIZE);
   if (uploader->bo != NULL && size_B <= KK_CMD_BO_SIZE - offset) {
      uploader->offset = offset + size_B;

      return (struct kk_ptr){
         .gpu = uploader->bo->gpu + offset,
         .cpu = uploader->bo->cpu + offset,

         .buffer = uploader->bo->map,
         .offset = offset,
      };
   }

   struct kk_cmd_bo *bo;
   const VkResult result = kk_cmd_buffer_alloc_bo(cmd, &bo);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return (struct kk_ptr){0};
   }

   /* Pick whichever of the current upload BO and the new BO will have more
    * room left to be the BO for the next upload.  If our upload size is
    * bigger than the old offset, we're better off burning the whole new
    * upload BO on this one allocation and continuing on the current upload
    * BO.
    */
   if (uploader->bo == NULL || size_B < uploader->offset) {
      uploader->bo = bo->bo;
      uploader->offset = size_B;
   }

   return (struct kk_ptr){
      .gpu = bo->bo->gpu,
      .cpu = bo->bo->cpu,

      .buffer = bo->bo->map,
      .offset = 0u,
   };
}

struct kk_ptr
kk_pool_upload(struct kk_cmd_buffer *cmd, const void *data, uint32_t size,
               uint32_t alignment)
{
   struct kk_ptr T = kk_pool_alloc(cmd, size, alignment);
   if (unlikely(T.cpu == NULL))
      return (struct kk_ptr){0};

   memcpy(T.cpu, data, size);
   return T;
}

static struct kk_ptr
kk_upload_descriptor_root_ptr(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size)
{
   struct kk_ptr ptr = kk_pool_upload(cmd, root, size, 8u);
   if (unlikely(!ptr.gpu)) return (struct kk_ptr){0};
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   ((struct kk_root_descriptor_table *)ptr.cpu)->sparse_state = kk_sparse_device_address(dev);
   ((struct kk_root_descriptor_table *)ptr.cpu)->addr =
      dev->atomic64_locks ? dev->atomic64_locks->gpu : ptr.gpu;
   return ptr;
}

uint64_t
kk_upload_descriptor_root_data(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size)
{
   return kk_upload_descriptor_root_ptr(cmd, root, size).gpu;
}

uint64_t
kk_upload_descriptor_root(struct kk_cmd_buffer *cmd,
                          VkPipelineBindPoint bind_point)
{
   struct kk_descriptor_state *desc = kk_get_descriptors_state(cmd, bind_point);
   struct kk_root_descriptor_table *root = &desc->root;
   if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && cmd->state.gfx.generated_root) {
      root->addr = cmd->state.gfx.generated_root;
      desc->root_dirty = false;
      return root->addr;
   }
   uint32_t size = offsetof(struct kk_root_descriptor_table, push) + desc->push_size;
   struct kk_ptr ptr = kk_upload_descriptor_root_ptr(cmd, root, size);
   if (!ptr.gpu) return 0u;
   kk_descriptor_root_apply_heap(desc, ptr.cpu);
   root->addr = ptr.gpu;
   desc->root_dirty = false;
   return ptr.gpu;
}

void
kk_cmd_buffer_flush_push_descriptors(struct kk_cmd_buffer *cmd,
                                     struct kk_descriptor_state *desc)
{
   u_foreach_bit(set_idx, desc->push_dirty) {
      struct kk_push_descriptor_set *push_set = desc->push[set_idx];
      uint32_t size = push_set->layout->non_variable_descriptor_buffer_size;
      struct kk_ptr push_gpu = kk_pool_upload(
         cmd, push_set->data, size, KK_MIN_UBO_ALIGNMENT);
      if (unlikely(!push_gpu.gpu))
         return;

      desc->root.sets[set_idx] = push_gpu.gpu;
      desc->set_sizes[set_idx] = size;
   }

   desc->root_dirty = true;
   desc->push_dirty = 0;
}

void
kk_dispatch_precomp(struct kk_cmd_buffer *cmd, struct kk_grid grid,
                    bool pre_gfx, enum libkk_program idx, void *data,
                    size_t data_size)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_precompiled_shader *prog = &dev->precompiled_cache.shaders[idx];

   mtl_compute_encoder *encoder = cs_get_compute(cmd);
   mtl_barrier_after_encoder_stages(encoder, MTL_STAGE_DISPATCH,
                                    MTL_STAGE_DISPATCH);

   struct kk_ptr data_gpu = kk_pool_upload(cmd, data, data_size, 8u);
   if (unlikely(!data_gpu.gpu))
      return;

   mtl_set_address(cmd->argument_table, data_gpu.gpu, KK_ARGUMENT_ROOT);
   mtl_compute_set_pipeline_state(encoder, prog->pipeline);

   struct mtl_size local_size = {
      .x = prog->info.workgroup_size[0],
      .y = prog->info.workgroup_size[1],
      .z = prog->info.workgroup_size[2],
   };

   if (grid.mode == KK_GRID_DIRECT)
      mtl_dispatch_threads(encoder, grid.size, local_size);
   else if (grid.mode == KK_GRID_INDIRECT_LOCAL)
      mtl_dispatch_threads_with_indirect_buffer(encoder, grid.addr);
   else
      mtl_dispatch_threadgroups_with_indirect_buffer(encoder, grid.addr,
                                                     local_size);
   mtl_barrier_after_encoder_stages(encoder, MTL_STAGE_DISPATCH,
                                    MTL_STAGE_DISPATCH);

   /* Rebind the existing root. */
   mtl_set_address(cmd->argument_table, cmd->state.root_addr, KK_ARGUMENT_ROOT);
}

void
kk_cmd_write(struct kk_cmd_buffer *cmd, struct libkk_imm_write write)
{
   /* If we are mid render, queue the write for when we are done */
   if (cmd->metal.render)
      util_dynarray_append(&cmd->post_render_writes, write);
   else
      libkk_write_u32(cmd, kk_grid_1d(1), true, write.address, write.value);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdPushDescriptorSetWithTemplate2KHR(
   VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfoKHR
                                     *pPushDescriptorSetWithTemplateInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(vk_descriptor_update_template, template,
                  pPushDescriptorSetWithTemplateInfo->descriptorUpdateTemplate);
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout,
                  pPushDescriptorSetWithTemplateInfo->layout);

   struct kk_descriptor_state *desc =
      kk_get_descriptors_state(cmd, template->bind_point);
   struct kk_descriptor_set_layout *set_layout = vk_to_kk_descriptor_set_layout(
      pipeline_layout->set_layouts[pPushDescriptorSetWithTemplateInfo->set]);
   struct kk_push_descriptor_set *push_set = kk_cmd_push_descriptors(
      cmd, desc, set_layout, pPushDescriptorSetWithTemplateInfo->set);
   if (unlikely(push_set == NULL))
      return;

   kk_push_descriptor_set_update_template(
      push_set, set_layout, template,
      pPushDescriptorSetWithTemplateInfo->pData);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginConditionalRendering2EXT(
   VkCommandBuffer commandBuffer,
   const VkConditionalRenderingBeginInfo2EXT *pConditionalRenderingBegin)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   cmd->state.cond_render.address =
      pConditionalRenderingBegin->addressRange.address;
   cmd->state.cond_render.inverted = pConditionalRenderingBegin->flags &
                                     VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;
   cmd->state.cond_render.enabled = true;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   cmd->state.cond_render.enabled = false;
}

static enum mtl_store_action
kk_get_attachment_store_op(const struct kk_attachment *attachment,
                           bool force_store,
                           bool rendering_to_whole_framebuffer,
                           VkImageAspectFlags aspect)
{
   if (attachment->iview && aspect == VK_IMAGE_ASPECT_COLOR_BIT)
      force_store |= container_of(attachment->iview->vk.image,
                                   struct kk_image, vk)->writable_msaa != NULL;
   bool renderpass_resolve = kk_attachment_do_renderpass_resolve(
      attachment, rendering_to_whole_framebuffer, aspect);
   force_store |=
      attachment->resolve_mode != VK_RESOLVE_MODE_NONE && !renderpass_resolve;

   force_store |= (attachment->load_op == VK_ATTACHMENT_LOAD_OP_LOAD ||
                   attachment->load_op == VK_ATTACHMENT_LOAD_OP_NONE) &&
                  attachment->store_op == VK_ATTACHMENT_STORE_OP_NONE;

   enum mtl_store_action store_action =
      force_store
         ? MTL_STORE_ACTION_STORE
         : vk_attachment_store_op_to_mtl_store_action(attachment->store_op);

   if (renderpass_resolve) {
      if (store_action == MTL_STORE_ACTION_STORE)
         store_action = MTL_STORE_ACTION_STORE_AND_MULTISAMPLE_RESOLVE;
      else if (store_action == MTL_STORE_ACTION_DONT_CARE)
         store_action = MTL_STORE_ACTION_MULTISAMPLE_RESOLVE;
   }

   return store_action;
}

void
kk_apply_attachment_store_ops(struct kk_cmd_buffer *cmd, bool force_store)
{
   if (!cmd->metal.render)
      return;

   struct kk_rendering_state *render = &cmd->state.gfx.render;
   mtl_render_encoder *encoder = cs_get_render(cmd);

   force_store |= render->force_attachment_store;

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      uint32_t logical_index = cmd->state.gfx.render.color_map[i];

      if (render->color_att[i].iview &&
          logical_index != MESA_VK_ATTACHMENT_UNUSED) {
         enum mtl_store_action store_action = kk_get_attachment_store_op(
            &render->color_att[i], force_store,
            render->rendering_to_whole_framebuffer, VK_IMAGE_ASPECT_COLOR_BIT);

         mtl_render_set_color_store_action(encoder, store_action,
                                           logical_index);
      }
   }
   enum mtl_store_action depth_store_action = MTL_STORE_ACTION_UNKNOWN;
   enum mtl_store_action stencil_store_action = MTL_STORE_ACTION_UNKNOWN;
   if (render->depth_att.iview) {
      depth_store_action = kk_get_attachment_store_op(
         &render->depth_att, force_store,
         render->rendering_to_whole_framebuffer, VK_IMAGE_ASPECT_DEPTH_BIT);
   }
   if (render->stencil_att.iview) {
      stencil_store_action = kk_get_attachment_store_op(
         &render->stencil_att, force_store,
         render->rendering_to_whole_framebuffer, VK_IMAGE_ASPECT_STENCIL_BIT);
   }
   bool ds_resolve_dont_care =
      depth_store_action == MTL_STORE_ACTION_MULTISAMPLE_RESOLVE ||
      stencil_store_action == MTL_STORE_ACTION_MULTISAMPLE_RESOLVE;
   bool ds_store = depth_store_action == MTL_STORE_ACTION_STORE ||
                   stencil_store_action == MTL_STORE_ACTION_STORE;
   /* Metal does not allow combining MTL_STORE_ACTION_MULTISAMPLE_RESOLVE
    * (so resolve but don't care about the MS data) for depth/stencil with
    * MTL_STORE_ACTION_STORE for the other one of depth/stencil regardless of
    * whether it's the same texture or different ones. */
   bool ds_force_ms_store = ds_resolve_dont_care && ds_store;

   if (render->depth_att.iview) {
      if (depth_store_action == MTL_STORE_ACTION_MULTISAMPLE_RESOLVE &&
          ds_force_ms_store)
         depth_store_action = MTL_STORE_ACTION_STORE_AND_MULTISAMPLE_RESOLVE;

      mtl_render_set_depth_store_action(encoder, depth_store_action);
   }
   if (render->stencil_att.iview) {
      if (stencil_store_action == MTL_STORE_ACTION_MULTISAMPLE_RESOLVE &&
          ds_force_ms_store)
         stencil_store_action = MTL_STORE_ACTION_STORE_AND_MULTISAMPLE_RESOLVE;

      mtl_render_set_stencil_store_action(encoder, stencil_store_action);
   }
}

bool
kk_attachment_do_renderpass_resolve(const struct kk_attachment *attachment,
                                    bool rendering_to_whole_framebuffer,
                                    VkImageAspectFlags aspect)
{
   if (!rendering_to_whole_framebuffer)
      return false;

   if (attachment->resolve_mode == VK_RESOLVE_MODE_NONE ||
       !attachment->resolve_iview || !attachment->iview)
      return false;

   /* There appears to be a synchronization bug in Metal.
    * Producer barriers do not synchronize depth render pass resolves
    * properly, even when it's STAGE_ALL -> STAGE_ALL.
    * Force meta resolves for depth and stencil for now. */
   if (!(attachment->resolve_iview->vk.aspects & VK_IMAGE_ASPECT_COLOR_BIT))
      return false;

   /* Dynamic rendering doesn't allow different formats but the Mesa Vulkan
    * runtime does it anyway when emulating legacy render passes.
    * Just fall back to meta resolves instead of creating views on the fly. */
   if (attachment->vk_format != attachment->resolve_iview->vk.view_format)
      return false;

   if (attachment->flags &
       VK_RENDERING_ATTACHMENT_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR)
      return false;

   enum pipe_format p_format = vk_format_to_pipe_format(attachment->vk_format);
   const struct kk_va_format *format_features = kk_get_va_format(p_format);

   if (!format_features->resolve)
      return false;

   /* From the spec:
    * The aspectMask of any image view specified for pDepthAttachment or
    * pStencilAttachment is ignored. Instead, depth attachments are
    * automatically treated as if VK_IMAGE_ASPECT_DEPTH_BIT was specified
    * for their aspect masks, and stencil attachments are automatically
    * treated as if VK_IMAGE_ASPECT_STENCIL_BIT was specified for their
    * aspect masks.
    *
    * That's why we have an extra parameter for what we use the attachment for.
    */

   if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
      return attachment->resolve_mode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT ||
             attachment->resolve_mode == VK_RESOLVE_MODE_MIN_BIT ||
             attachment->resolve_mode == VK_RESOLVE_MODE_MAX_BIT;

   } else if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
      return attachment->resolve_mode == VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
   } else {
      return attachment->resolve_mode == VK_RESOLVE_MODE_AVERAGE_BIT;
   }
}

/* VK_AMD_buffer_marker */
VKAPI_ATTR void VKAPI_CALL
kk_CmdWriteMarkerToMemoryAMD(VkCommandBuffer commandBuffer,
                             const VkMemoryMarkerInfoAMD *pInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd_buffer, commandBuffer);
   struct libkk_imm_write write;

   /* If we are inside a compute encoder, we can directly encode the write but
    * we need the barrier. If we are mid render, we will queue it for when it is
    * closed. */
   if (cmd_buffer->metal.compute) {
      mtl_barrier_after_encoder_stages(cmd_buffer->metal.compute,
                                       MTL_STAGE_DISPATCH | MTL_STAGE_BLIT,
                                       MTL_STAGE_DISPATCH | MTL_STAGE_BLIT);
   }

   write.value = pInfo->marker;
   write.address = pInfo->dstRange.address;
   kk_cmd_write(cmd_buffer, write);
}

void
kk_cmd_buffer_set_label(struct kk_cmd_buffer *cmd, const char *label)
{
   if (cmd->metal.cmd_buf)
      mtl_command_buffer_set_label(cmd->metal.cmd_buf, label);

   if (cmd->metal.compute)
      mtl_encoder_set_label(cmd->metal.compute, label);

   if (cmd->metal.render)
      mtl_encoder_set_label(cmd->metal.render, label);
}

/* VK_EXT_debug_utils */
VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginDebugUtilsLabelEXT(VkCommandBuffer _commandBuffer,
                              const VkDebugUtilsLabelEXT *pLabelInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, _commandBuffer);

   vk_common_CmdBeginDebugUtilsLabelEXT(_commandBuffer, pLabelInfo);

   if (cmd->metal.render)
      mtl_encoder_push_debug_group(cmd->metal.render, pLabelInfo->pLabelName);

   if (cmd->metal.compute)
      mtl_encoder_push_debug_group(cmd->metal.compute, pLabelInfo->pLabelName);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdEndDebugUtilsLabelEXT(VkCommandBuffer _commandBuffer)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, _commandBuffer);
   vk_common_CmdEndDebugUtilsLabelEXT(_commandBuffer);

   if (cmd->metal.render)
      mtl_encoder_pop_debug_group(cmd->metal.render);

   if (cmd->metal.compute)
      mtl_encoder_pop_debug_group(cmd->metal.compute);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdInsertDebugUtilsLabelEXT(VkCommandBuffer _commandBuffer,
                               const VkDebugUtilsLabelEXT *pLabelInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, _commandBuffer);

   /* We purposely don't call the common implementation here. It makes
    * debug regions that last until the next vkCmdInsertDebugUtilsLabelEXT()
    * or vkCmdBeginDebugUtilsLabelEXT() call.
    * The Metal debug signpost does not need this, and it interferes
    * with propagating the begin/end debug regions to all of the
    * Metal command buffers and encoders. */
   if (cmd->metal.render)
      mtl_encoder_insert_debug_signpost(cmd->metal.render,
                                        pLabelInfo->pLabelName);

   if (cmd->metal.compute)
      mtl_encoder_insert_debug_signpost(cmd->metal.compute,
                                        pLabelInfo->pLabelName);
}

#include "kk_whole_pass.h"
