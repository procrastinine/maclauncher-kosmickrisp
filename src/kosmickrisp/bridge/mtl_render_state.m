/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_render_state.h"
#include "mtl_replay_autorelease.h"

#include "mtl_format.h"

/* TODO_KOSMICKRISP Remove */
#include "vk_to_mtl_map.h"

/* TODO_KOSMICKRISP Remove */
#include "vulkan/vulkan.h"

#include <Metal/MTL4RenderPass.h>
#include <Metal/MTLDepthStencil.h>
#include <Metal/MTLTexture.h>

/* Render pass descriptor */
mtl_render_pass_descriptor *
mtl_new_render_pass_descriptor(void)
{
   KK_MTL_REPLAY_POOL({
      return [MTL4RenderPassDescriptor new];
   });
}

static struct mtl_render_pass_attachment_info
mtl_render_pass_attachment_get_info(MTLRenderPassAttachmentDescriptor *attachment)
{
   id<MTLTexture> texture = attachment.texture;
   id<MTLTexture> base = texture;
   while (base.parentTexture)
      base = base.parentTexture;
   return (struct mtl_render_pass_attachment_info) {
      .present = texture != nil,
      .format = texture ? (uint32_t)texture.pixelFormat : 0,
      .samples = texture ? (uint32_t)texture.sampleCount : 0,
      .load = (enum mtl_load_action)attachment.loadAction,
      .store = (enum mtl_store_action)attachment.storeAction,
      .texture = (const void *)texture,
      .base_texture = (const void *)base,
   };
}

void
mtl_render_pass_descriptor_get_info(mtl_render_pass_descriptor *descriptor,
                                     struct mtl_render_pass_info *info)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      *info = (struct mtl_render_pass_info) {
         .default_samples = (uint32_t)desc.defaultRasterSampleCount,
         .width = (uint32_t)desc.renderTargetWidth,
         .height = (uint32_t)desc.renderTargetHeight,
      };
      for (uint32_t i = 0; i < 8; i++)
         info->attachments[i] =
            mtl_render_pass_attachment_get_info(desc.colorAttachments[i]);
      info->attachments[8] =
         mtl_render_pass_attachment_get_info(desc.depthAttachment);
      info->attachments[9] =
         mtl_render_pass_attachment_get_info(desc.stencilAttachment);
   });
}

mtl_render_pass_attachment_descriptor *
mtl_render_pass_descriptor_get_color_attachment(
   mtl_render_pass_descriptor *descriptor, uint32_t index)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      return desc.colorAttachments[index];
   });
}

mtl_render_pass_attachment_descriptor *
mtl_render_pass_descriptor_get_depth_attachment(
   mtl_render_pass_descriptor *descriptor)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      return desc.depthAttachment;
   });
}

mtl_render_pass_attachment_descriptor *
mtl_render_pass_descriptor_get_stencil_attachment(
   mtl_render_pass_descriptor *descriptor)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      return desc.stencilAttachment;
   });
}

void
mtl_render_pass_attachment_descriptor_set_common(
   mtl_render_pass_attachment_descriptor *descriptor, mtl_texture *texture,
   uint32_t level, uint32_t slice, bool set_subresource,
   enum mtl_load_action load_action)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc =
         (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.texture = (id<MTLTexture>)texture;
      if (set_subresource) {
         desc.level = level;
         desc.slice = slice;
      }
      desc.loadAction = (MTLLoadAction)load_action;
      desc.storeAction = MTLStoreActionUnknown;
   });
}

void
mtl_render_pass_attachment_descriptor_set_texture(
   mtl_render_pass_attachment_descriptor *descriptor, mtl_texture *texture)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.texture = (id<MTLTexture>)texture;
   });
}

void
mtl_render_pass_attachment_descriptor_set_level(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t level)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.level = level;
   });
}

void
mtl_render_pass_attachment_descriptor_set_slice(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t slice)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.slice = slice;
   });
}

void
mtl_render_pass_attachment_descriptor_set_load_action(
   mtl_render_pass_attachment_descriptor *descriptor,
   enum mtl_load_action action)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.loadAction = (MTLLoadAction)action;
   });
}

void
mtl_render_pass_attachment_descriptor_set_store_action(
   mtl_render_pass_attachment_descriptor *descriptor,
   enum mtl_store_action action)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.storeAction = (MTLStoreAction)action;
   });
}

void
mtl_render_pass_attachment_descriptor_set_clear_color(
   mtl_render_pass_attachment_descriptor *descriptor,
   struct mtl_clear_color clear_color)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassColorAttachmentDescriptor *desc = (MTLRenderPassColorAttachmentDescriptor *)descriptor;
      desc.clearColor = MTLClearColorMake(clear_color.red, clear_color.green, clear_color.blue, clear_color.alpha);
   });
}

void
mtl_render_pass_attachment_descriptor_set_clear_depth(
   mtl_render_pass_attachment_descriptor *descriptor, double depth)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassDepthAttachmentDescriptor *desc = (MTLRenderPassDepthAttachmentDescriptor *)descriptor;
      desc.clearDepth = depth;
   });
}

void
mtl_render_pass_attachment_descriptor_set_clear_stencil(mtl_render_pass_attachment_descriptor *descriptor,
                                                        uint32_t stencil)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassStencilAttachmentDescriptor *desc = (MTLRenderPassStencilAttachmentDescriptor *)descriptor;
      desc.clearStencil = stencil;
   });
}

void
mtl_render_pass_attachment_descriptor_set_depth_resolve_filter(mtl_render_pass_attachment_descriptor *descriptor,
                                                               enum mtl_depth_resolve_filter filter)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassDepthAttachmentDescriptor *desc = (MTLRenderPassDepthAttachmentDescriptor *)descriptor;
      desc.depthResolveFilter = (MTLMultisampleDepthResolveFilter)filter;
   });
}

void
mtl_render_pass_attachment_descriptor_set_stencil_resolve_filter(mtl_render_pass_attachment_descriptor *descriptor,
                                                                 enum mtl_stencil_resolve_filter filter)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassStencilAttachmentDescriptor *desc = (MTLRenderPassStencilAttachmentDescriptor *)descriptor;
      desc.stencilResolveFilter = (MTLMultisampleStencilResolveFilter)filter;
   });
}

void
mtl_render_pass_attachment_descriptor_set_resolve_texture(mtl_render_pass_attachment_descriptor *descriptor,
                                                          mtl_texture *resolve_texture)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.resolveTexture = resolve_texture;
   });
}

void
mtl_render_pass_attachment_descriptor_set_resolve_level(mtl_render_pass_attachment_descriptor *descriptor,
                                                        uint32_t resolve_level)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.resolveLevel = resolve_level;
   });
}

void
mtl_render_pass_attachment_descriptor_set_resolve_slice(mtl_render_pass_attachment_descriptor *descriptor,
                                                        uint32_t resolve_slice)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.resolveSlice = resolve_slice;
   });
}

void
mtl_render_pass_attachment_descriptor_set_resolve_depth_plane(mtl_render_pass_attachment_descriptor *descriptor,
                                                              uint32_t resolve_depth_plane)
{
   KK_MTL_REPLAY_POOL({
      MTLRenderPassAttachmentDescriptor *desc = (MTLRenderPassAttachmentDescriptor *)descriptor;
      desc.resolveDepthPlane = resolve_depth_plane;
   });
}

void
mtl_render_pass_descriptor_set_render_target_array_length(mtl_render_pass_descriptor *descriptor,
                                                          uint32_t length)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      desc.renderTargetArrayLength = length;
   });
}

void
mtl_render_pass_descriptor_set_render_target_width(mtl_render_pass_descriptor *descriptor,
                                                   uint32_t width)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      desc.renderTargetWidth = width;
   });
}

void
mtl_render_pass_descriptor_set_render_target_height(mtl_render_pass_descriptor *descriptor,
                                                    uint32_t height)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      desc.renderTargetHeight = height;
   });
}

void
mtl_render_pass_descriptor_set_default_raster_sample_count(mtl_render_pass_descriptor *descriptor,
                                                           uint32_t sample_count)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      desc.defaultRasterSampleCount = sample_count;
   });
}

void
mtl_render_pass_descriptor_set_sample_positions(
   mtl_render_pass_descriptor *descriptor,
   const struct mtl_sample_position *positions,
   uint32_t count)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      if (count > 0) {
         MTLSamplePosition pos[count];
         for (uint32_t i = 0; i < count; i++) {
            pos[i] = MTLSamplePositionMake(positions[i].x, positions[i].y);
         }
         [desc setSamplePositions:pos count:count];
      } else {
         [desc setSamplePositions:NULL count:count];
      }
   });
}

void
mtl_render_pass_descriptor_set_visibility_buffer(mtl_render_pass_descriptor *descriptor,
                                                 mtl_buffer *visibility_buffer)
{
   KK_MTL_REPLAY_POOL({
      MTL4RenderPassDescriptor *desc = (MTL4RenderPassDescriptor *)descriptor;
      id<MTLBuffer> buffer = (id<MTLBuffer>)visibility_buffer;
      desc.visibilityResultBuffer = buffer;
   });
}

/* Stencil descriptor */
mtl_stencil_descriptor *
mtl_new_stencil_descriptor()
{
   KK_MTL_REPLAY_POOL({
      return [MTLStencilDescriptor new];
   });
}

/* TODO_KOSMICKRISP Move this to map */
static MTLStencilOperation
map_vk_stencil_op_to_mtl_stencil_operation(VkStencilOp op)
{
   switch (op) {
   case VK_STENCIL_OP_KEEP:
      return MTLStencilOperationKeep;
   case VK_STENCIL_OP_ZERO:
      return MTLStencilOperationZero;
   case VK_STENCIL_OP_REPLACE:
      return MTLStencilOperationReplace;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
      return MTLStencilOperationIncrementClamp;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
      return MTLStencilOperationDecrementClamp;
   case VK_STENCIL_OP_INVERT:
      return MTLStencilOperationInvert;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:
      return MTLStencilOperationIncrementWrap;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:
      return MTLStencilOperationDecrementWrap;
   default:
      assert(false && "Unsupported VkStencilOp");
      return MTLStencilOperationZero;
   };
}

void
mtl_stencil_descriptor_set_stencil_failure_operation(mtl_stencil_descriptor *descriptor, VkStencilOp op)
{
   KK_MTL_REPLAY_POOL({
      MTLStencilDescriptor *desc = (MTLStencilDescriptor *)descriptor;
      desc.stencilFailureOperation = map_vk_stencil_op_to_mtl_stencil_operation(op);
   });
}

void
mtl_stencil_descriptor_set_depth_failure_operation(mtl_stencil_descriptor *descriptor, VkStencilOp op)
{
   KK_MTL_REPLAY_POOL({
      MTLStencilDescriptor *desc = (MTLStencilDescriptor *)descriptor;
      desc.depthFailureOperation = map_vk_stencil_op_to_mtl_stencil_operation(op);
   });
}

void
mtl_stencil_descriptor_set_depth_stencil_pass_operation(mtl_stencil_descriptor *descriptor, VkStencilOp op)
{
   KK_MTL_REPLAY_POOL({
      MTLStencilDescriptor *desc = (MTLStencilDescriptor *)descriptor;
      desc.depthStencilPassOperation = map_vk_stencil_op_to_mtl_stencil_operation(op);
   });
}

void
mtl_stencil_descriptor_set_stencil_compare_function(mtl_stencil_descriptor *descriptor, VkCompareOp op)
{
   KK_MTL_REPLAY_POOL({
      MTLStencilDescriptor *desc = (MTLStencilDescriptor *)descriptor;
      desc.stencilCompareFunction = (MTLCompareFunction)vk_compare_op_to_mtl_compare_function(op);
   });
}

void
mtl_stencil_descriptor_set_read_mask(mtl_stencil_descriptor *descriptor, uint32_t mask)
{
   KK_MTL_REPLAY_POOL({
      MTLStencilDescriptor *desc = (MTLStencilDescriptor *)descriptor;
      desc.readMask = mask;
   });
}

void
mtl_stencil_descriptor_set_write_mask(mtl_stencil_descriptor *descriptor, uint32_t mask)
{
   KK_MTL_REPLAY_POOL({
      MTLStencilDescriptor *desc = (MTLStencilDescriptor *)descriptor;
      desc.writeMask = mask;
   });
}

/* Depth stencil descriptor */
mtl_depth_stencil_descriptor *
mtl_new_depth_stencil_descriptor()
{
   KK_MTL_REPLAY_POOL({
      return [MTLDepthStencilDescriptor new];
   });
}

void
mtl_depth_stencil_descriptor_set_depth_compare_function(mtl_depth_stencil_descriptor *descriptor, VkCompareOp op)
{
   KK_MTL_REPLAY_POOL({
      MTLDepthStencilDescriptor *desc = (MTLDepthStencilDescriptor *)descriptor;
      desc.depthCompareFunction = (MTLCompareFunction)vk_compare_op_to_mtl_compare_function(op);
   });
}

void
mtl_depth_stencil_descriptor_set_depth_write_enabled(mtl_depth_stencil_descriptor *descriptor, bool enable_write)
{
   KK_MTL_REPLAY_POOL({
      MTLDepthStencilDescriptor *desc = (MTLDepthStencilDescriptor *)descriptor;
      desc.depthWriteEnabled = enable_write;
   });
}

void
mtl_depth_stencil_descriptor_set_back_face_stencil(mtl_depth_stencil_descriptor *descriptor, mtl_stencil_descriptor *stencil_descriptor)
{
   KK_MTL_REPLAY_POOL({
      MTLDepthStencilDescriptor *desc = (MTLDepthStencilDescriptor *)descriptor;
      desc.backFaceStencil = (MTLStencilDescriptor *)stencil_descriptor;
   });
}

void
mtl_depth_stencil_descriptor_set_front_face_stencil(mtl_depth_stencil_descriptor *descriptor, mtl_stencil_descriptor *stencil_descriptor)
{
   KK_MTL_REPLAY_POOL({
      MTLDepthStencilDescriptor *desc = (MTLDepthStencilDescriptor *)descriptor;
      desc.frontFaceStencil = (MTLStencilDescriptor *)stencil_descriptor;
   });
}

mtl_depth_stencil_state *
mtl_new_depth_stencil_state(mtl_device *device, mtl_depth_stencil_descriptor *descriptor)
{
   KK_MTL_REPLAY_POOL({
      id<MTLDevice> dev = (id<MTLDevice>)device;
      MTLDepthStencilDescriptor *desc = (MTLDepthStencilDescriptor *)descriptor;
      return [dev newDepthStencilStateWithDescriptor:desc];
   });
}
