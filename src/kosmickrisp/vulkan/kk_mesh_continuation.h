/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_CONTINUATION_H
#define KK_MESH_CONTINUATION_H

#include "kk_interlock_submit.h"
#include "kk_descriptor_set.h"

/* One live auxiliary command and allocator, independent of the indirect grid.
 * The existing ordered queue continuation gates the application tail. */
#define KK_MESH_CONTINUATION_GROUPS 32u
struct kk_mesh_continuation {
   struct kk_device *device;
   VkCommandPool pool;
   struct kk_cmd_buffer *cmd;
   __typeof__(((struct kk_cmd_buffer *)0)->state) state;
   struct vk_dynamic_graphics_state dynamic;
   struct kk_ptr snapshot;
   uint64_t indirect, count;
   uint32_t maximum, recorded_maximum, stride, draw, base, total, chunk;
   bool force_virtual, snapshot_pending;
   mtl_command_queue *queue;
   mtl_feedback_handler_callback completed;
   void *completed_data;
};

static void
kk_mesh_continuation_finish(struct kk_device *dev, void *data)
{
   struct kk_mesh_continuation *p = data;
   if (p->pool) kk_DestroyCommandPool(kk_device_to_handle(dev), p->pool, NULL);
   for (unsigned i = 0; i < KK_MAX_SETS; ++i)
      free(p->state.gfx.descriptors.push[i]);
   free(p);
}

static void
kk_mesh_continuation_step(struct kk_mesh_continuation *p);

static void
kk_mesh_continuation_complete(struct mtl_feedback_data *feedback)
{
   struct kk_mesh_continuation *p = feedback->user_data;
   if (feedback->error == MTL_COMMAND_QUEUE_ERROR_NONE &&
       !vk_device_is_lost(&p->device->vk)) {
      if (p->snapshot_pending) {
         const uint32_t *g = p->snapshot.cpu;
         p->maximum = MIN2(p->maximum, g[3]);
         p->total = (uint64_t)g[0] * g[1] * g[2];
         p->base = 0;
         p->snapshot_pending = false;
      } else {
         p->base += p->chunk;
         if (p->base >= p->total) {
            ++p->draw;
            p->snapshot_pending = true;
         }
      }
      if (p->draw < p->maximum) {
         kk_mesh_continuation_step(p);
         return;
      }
   }
   struct mtl_feedback_data final = *feedback;
   final.user_data = p->completed_data;
   if (vk_device_is_lost(&p->device->vk) && !final.error)
      final.error = MTL_COMMAND_QUEUE_ERROR_INTERNAL;
   p->completed(&final);
}

static void
kk_mesh_continuation_step(struct kk_mesh_continuation *p)
{
   struct kk_cmd_buffer *cmd = p->cmd;
   VkResult result = kk_BeginCommandBuffer(kk_cmd_buffer_to_handle(cmd),
      &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT});
   if (result != VK_SUCCESS) goto fail;
   cmd->mesh_chunk.active = false;
   cmd->state = p->state;
   memset(&cmd->state.cs, 0, sizeof(cmd->state.cs));
   cmd->state.rt = NULL;
   cmd->state.gfx.depth_stencil_state = NULL;
   cmd->state.gfx.is_depth_stencil_dynamic = false;
   cmd->state.gfx.render_pass_descriptor = NULL;
   /* Own all mutable descriptor packets; the immutable original upload and
    * referenced Vulkan resources remain retained by the enclosing segment. */
   memset(cmd->state.gfx.descriptors.push, 0, sizeof(cmd->state.gfx.descriptors.push));
   for (unsigned i = 0; i < KK_MAX_SETS; ++i) {
      struct kk_push_descriptor_set *push = p->state.gfx.descriptors.push[i];
      if (!push) continue;
      size_t size = sizeof(*push) + push->capacity;
      cmd->state.gfx.descriptors.push[i] = vk_alloc(&kk_cmd_buffer_pool(cmd)->vk.alloc,
         size, 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!cmd->state.gfx.descriptors.push[i]) goto fail;
      memcpy(cmd->state.gfx.descriptors.push[i], push, size);
   }
   memset(&cmd->state.cs, 0, sizeof(cmd->state.cs));
   cmd->state.rt = NULL;
   cmd->state.gfx.depth_stencil_state = NULL;
   cmd->state.gfx.is_depth_stencil_dynamic = false;
   cmd->state.gfx.render_pass_descriptor = NULL;
   cmd->state.gfx.need_to_start_render_pass = true;
   cmd->state.gfx.mesh_payloads = cmd->state.gfx.mesh_records = 0;
   cmd->state.gfx.mesh_batch = cmd->state.gfx.mesh_slices = 0;
   cmd->state.gfx.mesh_native_batch = cmd->state.gfx.mesh_native_slices = 0;
   cmd->state.gfx.mesh_retained_outputs = cmd->state.gfx.mesh_retained_output_bytes = 0;
   cmd->vk.dynamic_graphics_state.vi = &cmd->state.gfx._dynamic_vi;
   cmd->vk.dynamic_graphics_state.ms.sample_locations = &cmd->state.gfx._dynamic_sl;
   vk_dynamic_graphics_state_copy(&cmd->vk.dynamic_graphics_state, &p->dynamic);
   kk_cmd_buffer_dirty_all_gfx(cmd);
   uint64_t grid = p->indirect + (uint64_t)p->draw * p->stride;
   if (p->snapshot_pending) {
      struct kk_mesh_params params = kk_mesh_parameters(cmd, grid, p->count,
         p->draw, p->maximum);
      uint64_t address = kk_pool_upload(cmd, &params, sizeof(params), 8).gpu;
      if (!address) goto fail;
      libkk_mesh_snapshot(cmd, kk_grid_1d(1), true, address, p->snapshot.gpu);
   } else if (p->draw < p->maximum && p->total > p->base) {
      p->chunk = MIN2(KK_MESH_CONTINUATION_GROUPS, p->total - p->base);
      cmd->mesh_chunk.active = true;
      cmd->mesh_chunk.groups = p->chunk;
      cmd->mesh_chunk.base = p->base;
      cmd->mesh_chunk.draw = p->draw;
      cmd->mesh_chunk.maximum = p->maximum;
      kk_draw_mesh(cmd, (struct mtl_size){0}, grid, p->count, 1, p->stride,
         p->force_virtual);
   } else {
      p->chunk = 0;
   }
   if (cmd->vk.record_result != VK_SUCCESS) goto fail;
   /* Store every partial attachment before the next chunk reloads it. */
   if (cmd->metal.render) kk_apply_attachment_store_ops(cmd, true);
   if (kk_EndCommandBuffer(kk_cmd_buffer_to_handle(cmd)) != VK_SUCCESS) goto fail;
   kk_device_make_resources_resident(p->device);
   mtl_commit_options *options = mtl_new_commit_options();
   if (!options) goto fail;
   mtl_commit_options_add_feedback_handler(options, kk_mesh_continuation_complete, p);
   mtl_command_queue_commit(p->queue, cmd->native_submit.buffers,
      cmd->native_submit.count, options);
   mtl_release(options);
   return;
fail:;
   struct mtl_feedback_data failure = {.user_data = p->completed_data,
      .error = MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY,
      .error_message = "Mesh continuation encoding failed"};
   p->completed(&failure);
}

static bool
kk_mesh_continuation_encode(void *data, mtl_command_queue *queue,
   mtl_feedback_handler_callback completed, void *completed_data)
{
   struct kk_mesh_continuation *p = data;
   if (vk_device_is_lost(&p->device->vk)) return false;
   p->queue = queue;
   p->completed = completed;
   p->completed_data = completed_data;
   p->draw = p->base = 0;
   const uint32_t *g = p->snapshot.cpu;
   p->maximum = MIN2(p->recorded_maximum, g[3]);
   p->total = (uint64_t)g[0] * g[1] * g[2];
   p->snapshot_pending = false;
   kk_mesh_continuation_step(p);
   return true;
}

static VkResult
kk_mesh_continue_indirect(struct kk_cmd_buffer *cmd, uint64_t indirect,
   uint64_t count, uint32_t maximum, uint32_t stride, bool force_virtual)
{
   struct kk_mesh_continuation *p = calloc(1, sizeof(*p));
   if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_device *dev = p->device = kk_cmd_buffer_device(cmd);
   kk_flush_gfx_state(cmd);
   p->state = cmd->state;
   memset(p->state.gfx.descriptors.push, 0, sizeof(p->state.gfx.descriptors.push));
   for (unsigned i = 0; i < KK_MAX_SETS; ++i) {
      struct kk_push_descriptor_set *push = cmd->state.gfx.descriptors.push[i];
      if (!push) continue;
      size_t size = sizeof(*push) + push->capacity;
      p->state.gfx.descriptors.push[i] = malloc(size);
      if (!p->state.gfx.descriptors.push[i]) goto fail;
      memcpy(p->state.gfx.descriptors.push[i], push, size);
   }
   /* The capture prefix already applies the original load/clear actions. */
   p->state.gfx.render.force_attachment_store = true;
   for (unsigned i = 0; i < p->state.gfx.render.color_att_count; ++i)
      p->state.gfx.render.color_att[i].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
   p->state.gfx.render.depth_att.load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
   p->state.gfx.render.stencil_att.load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
   p->dynamic = cmd->vk.dynamic_graphics_state;
   p->dynamic.vi = &p->state.gfx._dynamic_vi;
   p->dynamic.ms.sample_locations = &p->state.gfx._dynamic_sl;
   p->indirect = indirect; p->count = count;
   p->maximum = p->recorded_maximum = maximum; p->stride = stride; p->force_virtual = force_virtual;
   VkResult result = kk_CreateCommandPool(kk_device_to_handle(dev),
      &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = kk_cmd_buffer_pool(cmd)->vk.queue_family_index},
      NULL, &p->pool);
   if (result != VK_SUCCESS) goto fail;
   struct vk_command_buffer *vk_cmd;
   result = kk_cmd_buffer_ops.create(&kk_cmd_pool_from_handle(p->pool)->vk,
      VK_COMMAND_BUFFER_LEVEL_PRIMARY, &vk_cmd);
   if (result != VK_SUCCESS) goto fail;
   p->cmd = container_of(vk_cmd, struct kk_cmd_buffer, vk);
   p->snapshot = kk_pool_alloc(cmd, 16, 16);
   if (!p->snapshot.gpu) goto fail;
   struct kk_mesh_params params = kk_mesh_parameters(cmd, indirect, count, 0, maximum);
   uint64_t address = kk_pool_upload(cmd, &params, sizeof(params), 8).gpu;
   if (!address) goto fail;
   if (cmd->metal.render) kk_apply_attachment_store_ops(cmd, true);
   libkk_mesh_snapshot(cmd, kk_grid_1d(1), true, address, p->snapshot.gpu);
   result = kk_interlock_split(cmd, p, kk_mesh_continuation_encode,
      kk_mesh_continuation_finish);
   if (result == VK_SUCCESS) return result;
fail:
   kk_mesh_continuation_finish(dev, p);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}
#endif
