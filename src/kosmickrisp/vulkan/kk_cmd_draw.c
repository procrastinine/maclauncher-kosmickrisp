/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_entrypoints.h"
#include "kk_perf.h"
#include "kk_render_pass_cache.h"

#include "kk_buffer.h"
#include "kk_cmd_buffer.h"
#include "vk_common_entrypoints.h"
#include "kk_depth_stencil_state.h"
#include "kk_depth_bias_control.h"
#include "kk_depth_bounds.h"
#include "kk_interlock_heap.h"
#include "kk_interlock_draw.h"
#include "kk_interlock_envelope.h"
#include "vk_standard_sample_locations.h"
#include "kk-writable-msaa.h"
#include "kk_conservative.h"
#include "kk_dgc.h"
#include "kosmickrisp/libkk/kk_tess_grid.h"
#include "kk_format.h"
#include "kk_image_view.h"
#include "kk_layered_bc.h"
#include "kk_physical_device.h"
#include "kk_query_pool.h"
#include "kk_render_scissors.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/vk_to_mtl_map.h"

#include "kosmickrisp/libkk/kk_tessellator.h"

#include "poly/geometry.h"
#include "poly/tessellator.h"

#include "vulkan/runtime/vk_render_pass.h"
#include "vulkan/util/vk_format.h"

static void
kk_cmd_buffer_dirty_render_pass(struct kk_cmd_buffer *cmd)
{
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;

   /* These depend on color attachment count */
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_CB_COLOR_WRITE_ENABLES);
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_ENABLES);
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_CB_BLEND_EQUATIONS);
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_CB_WRITE_MASKS);

   /* These depend on the depth/stencil format */
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_TEST_ENABLE);
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_WRITE_ENABLE);
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_DS_DEPTH_BOUNDS_TEST_ENABLE);
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_DS_STENCIL_TEST_ENABLE);

   /* This may depend on render targets for ESO */
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES);
}

static void
kk_attachment_init(struct kk_attachment *att,
                   const VkRenderingAttachmentInfo *info)
{
   if (info == NULL || info->imageView == VK_NULL_HANDLE) {
      *att = (struct kk_attachment){
         .iview = NULL,
      };
      return;
   }

   VK_FROM_HANDLE(kk_image_view, iview, info->imageView);
   *att = (struct kk_attachment){
      .flags = vk_get_rendering_attachment_flags(info),
      .image_layout = info->imageLayout,
      .vk_format = iview->vk.format,
      .iview = iview,
   };

   if (info->resolveMode != VK_RESOLVE_MODE_NONE) {
      VK_FROM_HANDLE(kk_image_view, res_iview, info->resolveImageView);
      att->resolve_mode = info->resolveMode;
      att->resolve_iview = res_iview;
   }

   att->store_op = info->storeOp;
   att->load_op = info->loadOp;
   att->clear_value = info->clearValue;
}

VKAPI_ATTR void VKAPI_CALL
kk_GetRenderingAreaGranularityKHR(
   VkDevice device, const VkRenderingAreaInfoKHR *pRenderingAreaInfo,
   VkExtent2D *pGranularity)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   *pGranularity = (VkExtent2D){
      .width = pdev->info.rendering_tile_width,
      .height = pdev->info.rendering_tile_height,
   };
}

static void
kk_merge_render_iview(VkExtent2D *extent, struct kk_image_view *iview)
{
   if (iview) {
      /* TODO: is this right for ycbcr? */
      unsigned level = iview->vk.base_mip_level;
      unsigned width = u_minify(iview->vk.image->extent.width, level);
      unsigned height = u_minify(iview->vk.image->extent.height, level);

      if (iview->layered_bc) {
         width = iview->vk.extent.width;
         height = iview->vk.extent.height;
      }

      extent->width = MAX2(extent->width, width);
      extent->height = MAX2(extent->height, height);
   }
}

static void
kk_clear_common_attachment_description(
   mtl_render_pass_attachment_descriptor *descriptor)
{
   mtl_render_pass_attachment_descriptor_set_texture(descriptor, NULL);
   mtl_render_pass_attachment_descriptor_set_load_action(
      descriptor, MTL_LOAD_ACTION_DONT_CARE);
   mtl_render_pass_attachment_descriptor_set_store_action(
      descriptor, MTL_STORE_ACTION_UNKNOWN);
   mtl_render_pass_attachment_descriptor_set_resolve_texture(descriptor, NULL);
}

static enum mtl_load_action
kk_attachment_load_action(const struct kk_attachment *info, bool force_load)
{
   /* NONE with no writes must preserve the attachment just like LOAD. */
   if (force_load || info->load_op == VK_ATTACHMENT_LOAD_OP_LOAD ||
       (info->load_op == VK_ATTACHMENT_LOAD_OP_NONE &&
        info->store_op == VK_ATTACHMENT_STORE_OP_NONE))
      return MTL_LOAD_ACTION_LOAD;
   return vk_attachment_load_op_to_mtl_load_action(info->load_op);
}

static enum mtl_load_action
kk_fill_common_attachment_description(
   mtl_render_pass_attachment_descriptor *descriptor,
   const struct kk_attachment *info, const struct kk_rendering_state *render,
   bool force_attachment_load,
   bool attach_resolve_texture)
{
   const struct kk_image_view *iview = info->iview;
   assert(iview->plane_count ==
          1); /* TODO_KOSMICKRISP Handle multiplanar images? */
   enum mtl_load_action load_action =
      kk_attachment_load_action(info, force_attachment_load);

   /* A render view already selects its source subresource. Keep all base
    * writes in one bridge entry and preserve view/resolve override rules. */
   mtl_render_pass_attachment_descriptor_set_common(
      descriptor, kk_transient_render_texture(iview), iview->vk.base_mip_level,
      iview->vk.base_array_layer, !iview->planes[0].render_is_view, load_action);

   if (render->layered_bc)
      kk_layered_bc_attachment_description(descriptor, info, render, false);

   if (!attach_resolve_texture)
      mtl_render_pass_attachment_descriptor_set_resolve_texture(descriptor, NULL);

   if (attach_resolve_texture) {
      mtl_render_pass_attachment_descriptor_set_resolve_texture(
         descriptor, kk_transient_render_texture(info->resolve_iview));

      bool view = info->resolve_iview->planes[0].render_is_view;
      mtl_render_pass_attachment_descriptor_set_resolve_level(
         descriptor, view ? 0 : info->resolve_iview->vk.base_mip_level);
      mtl_render_pass_attachment_descriptor_set_resolve_slice(
         descriptor, view ? 0 : info->resolve_iview->vk.base_array_layer);
      mtl_render_pass_attachment_descriptor_set_resolve_depth_plane(descriptor, 0);
      if (render->layered_bc)
         kk_layered_bc_attachment_description(descriptor, info, render, true);
   }
   return load_action;
}

static struct mtl_clear_color
vk_clear_color_value_to_mtl_clear_color(union VkClearColorValue color,
                                        enum pipe_format format)
{
   struct mtl_clear_color value;
   if (util_format_is_pure_sint(format)) {
      value.red = color.int32[0];
      value.green = color.int32[1];
      value.blue = color.int32[2];
      value.alpha = color.int32[3];
   } else if (util_format_is_pure_uint(format)) {
      value.red = color.uint32[0];
      value.green = color.uint32[1];
      value.blue = color.uint32[2];
      value.alpha = color.uint32[3];
   } else {
      value.red = color.float32[0];
      value.green = color.float32[1];
      value.blue = color.float32[2];
      value.alpha = color.float32[3];
   }

   /* Apply swizzle to color since Metal does not allow swizzle for renderable
    * textures, but we need to support that for formats like
    * VK_FORMAT_B4G4R4A4_UNORM_PACK16 */
   const struct kk_va_format *supported_format = kk_get_va_format(format);
   struct mtl_clear_color swizzled_color = value;
   for (uint32_t i = 0u; i < 4; ++i) {
      unsigned channel = supported_format->unswizzle.channels[i];
      /* Constant view channels have no physical destination. */
      if (channel < 4)
         swizzled_color.channel[channel] = value.channel[i];
   }

   return swizzled_color;
}

static void
kk_set_color_attachments(struct kk_cmd_buffer *cmd,
                         mtl_render_pass_descriptor *pass_descriptor,
                         struct kk_rendering_state *render,
                         const struct vk_dynamic_graphics_state *dyn,
                         bool force_attachment_load,
                         bool rendering_to_whole_framebuffer,
                         bool clear_previous)
{
   /* Avoid materializing unused color slots on a new native descriptor.
    * A changed mapping reuses its descriptor and must clear old slots. */
   if (clear_previous) {
      for (uint32_t i = 0; i < ARRAY_SIZE(render->color_att); i++) {
         mtl_render_pass_attachment_descriptor *attachment_descriptor =
            kk_render_pass_color(cmd, pass_descriptor, i);
         kk_clear_common_attachment_description(attachment_descriptor);
      }
   }
   for (uint32_t i = 0; i < ARRAY_SIZE(render->color_att); i++) {
      struct kk_attachment *color_att = &render->color_att[i];
      const struct kk_image_view *iview = color_att->iview;
      uint8_t logical_index = dyn->cal.color_map[i];
      render->color_map[i] = logical_index;

      if (!iview || logical_index == MESA_VK_ATTACHMENT_UNUSED) {
         continue;
      }
      mtl_render_pass_attachment_descriptor *attachment_descriptor =
         kk_render_pass_color(cmd, pass_descriptor, logical_index);

      assert(iview->plane_count ==
             1); /* TODO_KOSMICKRISP Handle multiplanar images? */
      const struct kk_image *image =
         container_of(iview->vk.image, struct kk_image, vk);
      render->samples = MAX2(render->samples, image->vk.samples);

      bool renderpass_resolve = kk_attachment_do_renderpass_resolve(
         &render->color_att[i], rendering_to_whole_framebuffer,
         VK_IMAGE_ASPECT_COLOR_BIT);

      enum mtl_load_action load_action = kk_fill_common_attachment_description(
         attachment_descriptor, color_att, render, force_attachment_load,
         renderpass_resolve);

      if (load_action == MTL_LOAD_ACTION_CLEAR) {
         struct mtl_clear_color clear_color =
            vk_clear_color_value_to_mtl_clear_color(color_att->clear_value.color,
                                                    iview->planes[0].format);
         mtl_render_pass_attachment_descriptor_set_clear_color(
            attachment_descriptor, clear_color);
      }
   }
}

#include "kk_layered_bc_render.h"

/* Native extents include the render area's far corner. Keep partial-attachment
 * load/store preservation independent of this raster/tile bound. */
static VkExtent2D
kk_render_target_extent(VkExtent2D framebuffer, const VkRect2D *area)
{
   return (VkExtent2D) {
      .width = MAX2(MIN2((uint64_t)area->offset.x + area->extent.width,
                         framebuffer.width), 1u),
      .height = MAX2(MIN2((uint64_t)area->offset.y + area->extent.height,
                          framebuffer.height), 1u),
   };
}

VKAPI_ATTR void VKAPI_CALL
kk_begin_rendering(VkCommandBuffer commandBuffer,
                     const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   KK_PERF_ENCODER_SCOPE(dev, KK_PERF_ENCODER_SETUP, setup);
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;

   struct kk_rendering_state *render = &cmd->state.gfx.render;

   const bool layered_bc = kk_layered_bc_rendering_info_requested(pRenderingInfo);
   /* Per-layer physical passes cannot use whole-pass memoryless retention.
    * Reuse the existing private commitment path for paired transient views. */
   if (p_atomic_read(&dev->live_lazy_allocations)) {
      const bool transient_safe = cmd->transient_safe;
      cmd->transient_safe &= !layered_bc;
      VkResult result = kk_transient_prepare_rendering(cmd, pRenderingInfo);
      cmd->transient_safe = transient_safe;
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }
   }

   memset(render, 0, sizeof(*render));

   render->layered_bc = layered_bc;
   render->layered_bc_view_mask = pRenderingInfo->viewMask;
   render->flags = pRenderingInfo->flags;
   render->area = pRenderingInfo->renderArea;
   render->view_mask = pRenderingInfo->viewMask;
   render->layer_count = pRenderingInfo->layerCount;
   render->samples = 0;
   render->color_att_count = pRenderingInfo->colorAttachmentCount;

   const uint32_t layer_count = render->view_mask
                                   ? util_last_bit(render->view_mask)
                                   : render->layer_count;

   VkExtent2D framebuffer_extent = {.width = 0u, .height = 0u};
   bool does_any_attachment_clear = false;
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      kk_attachment_init(&render->color_att[i],
                         &pRenderingInfo->pColorAttachments[i]);
      kk_merge_render_iview(&framebuffer_extent, render->color_att[i].iview);
      does_any_attachment_clear |=
         (pRenderingInfo->pColorAttachments[i].loadOp ==
          VK_ATTACHMENT_LOAD_OP_CLEAR);
   }
   if (pRenderingInfo->pDepthAttachment)
      does_any_attachment_clear |= (pRenderingInfo->pDepthAttachment->loadOp ==
                                    VK_ATTACHMENT_LOAD_OP_CLEAR);
   if (pRenderingInfo->pStencilAttachment)
      does_any_attachment_clear |=
         (pRenderingInfo->pStencilAttachment->loadOp ==
          VK_ATTACHMENT_LOAD_OP_CLEAR);

   kk_attachment_init(&render->depth_att, pRenderingInfo->pDepthAttachment);
   kk_attachment_init(&render->stencil_att, pRenderingInfo->pStencilAttachment);
   kk_merge_render_iview(&framebuffer_extent,
                         render->depth_att.iview ?: render->stencil_att.iview);

   const VkRenderingFragmentShadingRateAttachmentInfoKHR *fsr_att_info =
      vk_find_struct_const(pRenderingInfo->pNext,
                           RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
   struct kk_descriptor_state *fsr_desc = &cmd->state.gfx.descriptors;
   if (fsr_att_info != NULL && fsr_att_info->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(kk_image_view, iview, fsr_att_info->imageView);
      render->fsr_att = (struct kk_attachment){
         .vk_format = iview->vk.format,
         .iview = iview,
         .store_op = VK_ATTACHMENT_STORE_OP_NONE,
      };
      fsr_desc->root.fsr.attachment = iview->planes[0].fsr_gpu_resource_id;
      fsr_desc->root_dirty = true;
   } else if (fsr_desc->root.fsr.attachment) {
      fsr_desc->root.fsr.attachment = 0;
      fsr_desc->root_dirty = true;
   }

   if (dev->vk.enabled_features.shaderStorageImageMultisample)
      kk_writable_msaa_init_render(cmd);

   kk_init_rendering_locations(cmd, pRenderingInfo);

   kk_cmd_buffer_dirty_render_pass(cmd);
   struct kk_perf_encoder_scope descriptor_scope
      __attribute__((cleanup(kk_perf_encoder_scope_cleanup)));
   descriptor_scope.perf = dev->perf;
   if (unlikely(descriptor_scope.perf)) {
      kk_perf_encoder_scope_begin(&descriptor_scope, KK_PERF_SETUP_DESCRIPTOR);
   }
   uint8_t color_mask = 0;
   for (unsigned i = 0; i < render->color_att_count; ++i)
      if (render->color_att[i].iview &&
          dyn->cal.color_map[i] != MESA_VK_ATTACHMENT_UNUSED)
         color_mask |= 1u << dyn->cal.color_map[i];
   mtl_render_pass_descriptor *pass_descriptor = kk_render_pass_acquire(cmd,
      color_mask, render->depth_att.iview != NULL, render->stencil_att.iview != NULL,
      cmd->uses_occlusion_queries ? dev->occlusion_queries.bo->map : NULL);
   if (!pass_descriptor) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   /* Framebufferless rendering, need to set pass_descriptors
    * renderTargetWidth/Height to non-0 values and defaultRasterSampleCount.
    * However, since the sample count will only be known at first pipeline bind,
    * we need to delay the start of the pass until then since Metal will ignore
    * bound pipeline's sample count. */
   bool no_framebuffer =
      framebuffer_extent.width == 0u && framebuffer_extent.height == 0u;
   render->attachmentless = no_framebuffer;
   if (no_framebuffer) {
      framebuffer_extent.width = MAX2(render->area.extent.width, 1u);
      framebuffer_extent.height = MAX2(render->area.extent.height, 1u);
      mtl_render_pass_descriptor_set_render_target_width(
         pass_descriptor, framebuffer_extent.width);
      mtl_render_pass_descriptor_set_render_target_height(
         pass_descriptor, framebuffer_extent.height);
      /* Applications may set maximal extents for attachment-less rendering, for
       * example DXVK. This can cause device loss from exceeding memory limits.
       * We can get around this by ignoring the layer count, without any known
       * impact on the resulting fragment invocations and side effects. */
      mtl_render_pass_descriptor_set_render_target_array_length(pass_descriptor,
                                                                0u);

      mtl_render_pass_descriptor_set_default_raster_sample_count(
         pass_descriptor, 1u);
   } else {
      const VkExtent2D target =
         kk_render_target_extent(framebuffer_extent, &render->area);
      mtl_render_pass_descriptor_set_render_target_width(pass_descriptor,
                                                          target.width);
      mtl_render_pass_descriptor_set_render_target_height(pass_descriptor,
                                                           target.height);

      /* Render targets are always arrays */
      mtl_render_pass_descriptor_set_render_target_array_length(
         pass_descriptor, render->layered_bc ? 1u : (layer_count ? layer_count : 1u));
   }

   /* Check if we are rendering to the whole framebuffer. Required to understand
    * if we need to load to avoid clearing all attachment when loading.
    */
   bool is_whole_framebuffer =
      framebuffer_extent.width == render->area.extent.width &&
      framebuffer_extent.height == render->area.extent.height &&
      render->area.offset.x == 0u && render->area.offset.y == 0u &&
      (render->view_mask == 0u ||
       render->view_mask == BITFIELD64_MASK(render->layer_count));

   render->rendering_to_whole_framebuffer = is_whole_framebuffer;

   /* renderTargetWidth/Height doesn't seem to guarantee
    * that the attachments won't get touched outside the area
    * so just checking for offset = 0 doesn't cut it. */
   bool force_attachment_load =
      !is_whole_framebuffer || (render->flags & VK_RENDERING_RESUMING_BIT);

   render->force_attachment_store =
      !is_whole_framebuffer || (render->flags & VK_RENDERING_SUSPENDING_BIT);

   /* A partial-area pass loads and stores whole tiles, including pixels
    * outside its render area. Vulkan needs no barrier between passes that
    * write disjoint areas of one attachment (atlas tiles; vk_meta records one
    * such pass per blit or resolve region), and a pass end is unordered, so
    * an earlier pass may still be storing those pixels. Order a partial-area
    * pass behind the earlier passes of this command buffer; the barrier is
    * applied when its render encoder starts. Full-area passes stay unordered. */
   if (!is_whole_framebuffer && cmd->render_pass_begun) {
      cmd->pending_barrier.src |= MTL_STAGE_FRAGMENT | MTL_STAGE_TILE;
      cmd->pending_barrier.dst |= MTL_STAGE_FRAGMENT | MTL_STAGE_TILE;
      cmd->pending_barrier.visibility =
         MAX2(cmd->pending_barrier.visibility, MTL_VISIBILITY_DEVICE);
   }
   cmd->render_pass_begun = true;

   kk_set_color_attachments(cmd, pass_descriptor, render, dyn, force_attachment_load,
                            is_whole_framebuffer, false);

   if (render->depth_att.iview) {
      const struct kk_image_view *iview = render->depth_att.iview;
      const struct kk_image *image =
         container_of(iview->vk.image, struct kk_image, vk);
      render->samples = image->vk.samples;

      bool renderpass_resolve = kk_attachment_do_renderpass_resolve(
         &render->depth_att, is_whole_framebuffer, VK_IMAGE_ASPECT_DEPTH_BIT);

      mtl_render_pass_attachment_descriptor *attachment_descriptor =
         kk_render_pass_depth(cmd, pass_descriptor);
      enum mtl_load_action load_action = kk_fill_common_attachment_description(
         attachment_descriptor, &render->depth_att, render, force_attachment_load,
         renderpass_resolve);

      /* clearValue.depthStencil.depth could have invalid values such as NaN
       * which will trigger a Metal validation error. Ensure we only use this
       * value if the attachment is actually cleared. */
      if (load_action == MTL_LOAD_ACTION_CLEAR)
         mtl_render_pass_attachment_descriptor_set_clear_depth(
            attachment_descriptor,
            pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth);

      if (renderpass_resolve)
         mtl_render_pass_attachment_descriptor_set_depth_resolve_filter(
            attachment_descriptor, vk_resolve_mode_to_mtl_depth_resolve_filter(
                                      render->depth_att.resolve_mode));
   }
   if (render->stencil_att.iview) {
      const struct kk_image_view *iview = render->stencil_att.iview;
      const struct kk_image *image =
         container_of(iview->vk.image, struct kk_image, vk);
      render->samples = image->vk.samples;

      bool renderpass_resolve = kk_attachment_do_renderpass_resolve(
         &render->stencil_att, is_whole_framebuffer,
         VK_IMAGE_ASPECT_STENCIL_BIT);

      mtl_render_pass_attachment_descriptor *attachment_descriptor =
         kk_render_pass_stencil(cmd, pass_descriptor);
      enum mtl_load_action load_action = kk_fill_common_attachment_description(
         attachment_descriptor, &render->stencil_att, render, force_attachment_load,
         renderpass_resolve);
      if (load_action == MTL_LOAD_ACTION_CLEAR)
         mtl_render_pass_attachment_descriptor_set_clear_stencil(
            attachment_descriptor,
            pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil);

      if (renderpass_resolve)
         mtl_render_pass_attachment_descriptor_set_stencil_resolve_filter(
            attachment_descriptor,
            vk_resolve_mode_to_mtl_stencil_resolve_filter(
               render->stencil_att.resolve_mode));
   }

   /* BeginQuery may occur after the first draw. Use the complete recording
    * summary, not the currently active query, before creating the encoder. */
   if (cmd->uses_occlusion_queries)
      mtl_render_pass_descriptor_set_visibility_buffer(
         pass_descriptor, dev->occlusion_queries.bo->map);

   // TODO_KOSMICKRISP Fragment shading rate support goes here if Metal supports
   // it

   /* Rendering with no attachments requires pushing the start of the render
    * pass to first pipeline binding to know sample count. */
   cmd->state.gfx.render_pass_descriptor = pass_descriptor;
   if (unlikely(descriptor_scope.perf)) {
      kk_perf_encoder_scope_end(&descriptor_scope);
      descriptor_scope.perf = NULL;
   }
   if (!no_framebuffer && !kk_layered_bc_start_rendering(cmd, force_attachment_load))
      return;

   /* Store descriptor in case we need to restart the pass at pipeline barrier,
    * but force loads */
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      const struct kk_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;
      /* Setup already emitted LOAD at this native slot. Other mappings keep
       * their existing restart writes instead of guessing a slot's contents. */
      if (render->color_map[i] == i &&
          kk_attachment_load_action(&render->color_att[i],
                                    force_attachment_load) == MTL_LOAD_ACTION_LOAD)
         continue;
      mtl_render_pass_attachment_descriptor *attachment_descriptor =
         kk_render_pass_color(cmd, pass_descriptor, i);
      mtl_render_pass_attachment_descriptor_set_load_action(
         attachment_descriptor, MTL_LOAD_ACTION_LOAD);
   }
   if (render->depth_att.iview &&
       kk_attachment_load_action(&render->depth_att, force_attachment_load) !=
          MTL_LOAD_ACTION_LOAD) {
      mtl_render_pass_attachment_descriptor *attachment_descriptor =
         kk_render_pass_depth(cmd, pass_descriptor);
      mtl_render_pass_attachment_descriptor_set_load_action(
         attachment_descriptor, MTL_LOAD_ACTION_LOAD);
   }
   if (render->stencil_att.iview &&
       kk_attachment_load_action(&render->stencil_att, force_attachment_load) !=
          MTL_LOAD_ACTION_LOAD) {
      mtl_render_pass_attachment_descriptor *attachment_descriptor =
         kk_render_pass_stencil(cmd, pass_descriptor);
      mtl_render_pass_attachment_descriptor_set_load_action(
         attachment_descriptor, MTL_LOAD_ACTION_LOAD);
   }
   cmd->state.gfx.need_to_start_render_pass = no_framebuffer || render->layered_bc;

   kk_cmd_buffer_invalidate_gfx_encoder(cmd);

   if (render->flags & VK_RENDERING_RESUMING_BIT)
      return;

   /* Clear attachments if we forced a load and there's a clear */
   if (!force_attachment_load || !does_any_attachment_clear)
      return;

   uint32_t clear_count = 0;
   VkClearAttachment clear_att[KK_MAX_RTS + 1];
   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att_info =
         &pRenderingInfo->pColorAttachments[i];
      if (att_info->imageView == VK_NULL_HANDLE ||
          att_info->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      clear_att[clear_count++] = (VkClearAttachment){
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .colorAttachment = i,
         .clearValue = att_info->clearValue,
      };
   }

   clear_att[clear_count] = (VkClearAttachment){
      .aspectMask = 0,
   };
   if (pRenderingInfo->pDepthAttachment != NULL &&
       pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE &&
       pRenderingInfo->pDepthAttachment->loadOp ==
          VK_ATTACHMENT_LOAD_OP_CLEAR) {
      clear_att[clear_count].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
      clear_att[clear_count].clearValue.depthStencil.depth =
         pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
   }
   if (pRenderingInfo->pStencilAttachment != NULL &&
       pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE &&
       pRenderingInfo->pStencilAttachment->loadOp ==
          VK_ATTACHMENT_LOAD_OP_CLEAR) {
      clear_att[clear_count].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      clear_att[clear_count].clearValue.depthStencil.stencil =
         pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
   }
   if (clear_att[clear_count].aspectMask != 0)
      clear_count++;

   if (clear_count > 0) {
      const VkClearRect clear_rect = {
         .rect = render->area,
         .baseArrayLayer = 0,
         .layerCount = render->view_mask ? 1 : render->layer_count,
      };

      /* This is the attachment load operation, which conditional rendering
       * never discards, unlike an application vkCmdClearAttachments. */
      const struct kk_conditional_rendering_state cond_render =
         cmd->state.cond_render;
      cmd->state.cond_render.enabled = false;
      kk_CmdClearAttachments(kk_cmd_buffer_to_handle(cmd), clear_count,
                             clear_att, 1, &clear_rect);
      cmd->state.cond_render = cond_render;
   }
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdEndRendering2KHR(VkCommandBuffer commandBuffer,
                       UNUSED const VkRenderingEndInfoKHR *pRenderingEndInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   KK_PERF_ENCODER_SCOPE(kk_cmd_buffer_device(cmd), KK_PERF_ENCODER_END, end);
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   bool need_meta_resolve = false;

   /* Translate render state back to VK for meta */
   VkRenderingAttachmentInfo vk_color_att[KK_MAX_RTS];
   VkRenderingAttachmentFlagsInfoKHR vk_color_att_flags[KK_MAX_RTS];
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      bool attachment_meta_resolve =
         render->color_att[i].resolve_mode != VK_RESOLVE_MODE_NONE &&
         !kk_attachment_do_renderpass_resolve(
            &render->color_att[i], render->rendering_to_whole_framebuffer,
            VK_IMAGE_ASPECT_COLOR_BIT);
      need_meta_resolve |= attachment_meta_resolve;

      vk_color_att_flags[i] = (VkRenderingAttachmentFlagsInfoKHR){
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR,
         .flags = render->color_att[i].flags,
      };
      vk_color_att[i] = (VkRenderingAttachmentInfo){
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .pNext = &vk_color_att_flags[i],
         .imageView = kk_image_view_to_handle(render->color_att[i].iview),
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .resolveMode = attachment_meta_resolve
                           ? render->color_att[i].resolve_mode
                           : VK_RESOLVE_MODE_NONE,
         .resolveImageView =
            attachment_meta_resolve
               ? kk_image_view_to_handle(render->color_att[i].resolve_iview)
               : VK_NULL_HANDLE,
         .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
   }

   bool depth_meta_resolve =
      render->depth_att.resolve_mode != VK_RESOLVE_MODE_NONE &&
      !kk_attachment_do_renderpass_resolve(
         &render->depth_att, render->rendering_to_whole_framebuffer,
         VK_IMAGE_ASPECT_DEPTH_BIT);
   need_meta_resolve |= depth_meta_resolve;

   const VkRenderingAttachmentFlagsInfoKHR vk_depth_att_flags = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR,
      .flags = render->depth_att.flags,
   };
   const VkRenderingAttachmentInfo vk_depth_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .pNext = &vk_depth_att_flags,
      .imageView = kk_image_view_to_handle(render->depth_att.iview),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = depth_meta_resolve ? render->depth_att.resolve_mode
                                        : VK_RESOLVE_MODE_NONE,
      .resolveImageView =
         depth_meta_resolve
            ? kk_image_view_to_handle(render->depth_att.resolve_iview)
            : VK_NULL_HANDLE,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };

   bool stencil_meta_resolve =
      render->stencil_att.resolve_mode != VK_RESOLVE_MODE_NONE &&
      !kk_attachment_do_renderpass_resolve(
         &render->stencil_att, render->rendering_to_whole_framebuffer,
         VK_IMAGE_ASPECT_STENCIL_BIT);
   need_meta_resolve |= stencil_meta_resolve;

   const VkRenderingAttachmentFlagsInfoKHR vk_stencil_att_flags = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR,
      .flags = render->stencil_att.flags,
   };
   const VkRenderingAttachmentInfo vk_stencil_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .pNext = &vk_stencil_att_flags,
      .imageView = kk_image_view_to_handle(render->stencil_att.iview),
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .resolveMode = stencil_meta_resolve ? render->stencil_att.resolve_mode
                                          : VK_RESOLVE_MODE_NONE,
      .resolveImageView =
         stencil_meta_resolve
            ? kk_image_view_to_handle(render->stencil_att.resolve_iview)
            : VK_NULL_HANDLE,
      .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };

   const VkRenderingInfo vk_render = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = render->area,
      .layerCount = render->layer_count,
      .viewMask = render->view_mask,
      .colorAttachmentCount = render->color_att_count,
      .pColorAttachments = vk_color_att,
      .pDepthAttachment = &vk_depth_att,
      .pStencilAttachment = &vk_stencil_att,
   };

   kk_apply_attachment_store_ops(cmd, false);

   /* A logical pass end has no implicit dependency on the next pass.
    * Internal resolves still consume these attachment writes. */
   if (need_meta_resolve) {
      cs_end(cmd);
   } else {
      cs_end_unordered(cmd);
      /* A partial-area pass stored whole tiles, including pixels outside its
       * render area that a later copy or dispatch may write without a
       * barrier. The next compute encoder waits for this store. */
      cmd->partial_pass_store_unordered |=
         !render->rendering_to_whole_framebuffer;
   }
   kk_render_pass_release(cmd, cmd->state.gfx.render_pass_descriptor);
   cmd->state.gfx.render_pass_descriptor = NULL;

   if (!render->msrtss && (render->flags &
       (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT)))
      need_meta_resolve = false;

   memset(render, 0, sizeof(*render));

   if (need_meta_resolve) {
      kk_meta_resolve_rendering(cmd, &vk_render);
   }
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginCustomResolveEXT(
   VkCommandBuffer commandBuffer,
   UNUSED const VkBeginCustomResolveInfoEXT *pBeginCustomResolveInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_rendering_state *render = &cmd->state.gfx.render;

   VkRenderingAttachmentInfo color_atts[KK_MAX_RTS];
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      color_atts[i] = (VkRenderingAttachmentInfo){
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      };

      if (render->color_att[i].resolve_mode != VK_RESOLVE_MODE_CUSTOM_BIT_EXT)
         continue;

      struct kk_image_view *iview = render->color_att[i].resolve_iview;

      color_atts[i].imageView = kk_image_view_to_handle(iview);
      color_atts[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   }

   VkRenderingAttachmentInfo depth_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
   };
   if (render->depth_att.resolve_mode == VK_RESOLVE_MODE_CUSTOM_BIT_EXT) {
      struct kk_image_view *iview = render->depth_att.resolve_iview;

      depth_att.imageView = kk_image_view_to_handle(iview);
      depth_att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   }

   VkRenderingAttachmentInfo stencil_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
   };
   if (render->stencil_att.resolve_mode == VK_RESOLVE_MODE_CUSTOM_BIT_EXT) {
      struct kk_image_view *iview = render->stencil_att.resolve_iview;

      stencil_att.imageView = kk_image_view_to_handle(iview);
      stencil_att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   }

   VkRenderingInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .flags = VK_RENDERING_LOCAL_READ_CONCURRENT_ACCESS_CONTROL_BIT_KHR,
      .renderArea = render->area,
      .layerCount = render->layer_count,
      .viewMask = render->view_mask,
      .colorAttachmentCount = render->color_att_count,
      .pColorAttachments = color_atts,
      .pDepthAttachment = &depth_att,
      .pStencilAttachment = &stencil_att,
   };

   const VkRenderingEndInfoKHR end_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_END_INFO_KHR,
   };

   kk_CmdEndRendering2KHR(commandBuffer, &end_info);
   kk_begin_rendering(commandBuffer, &rendering_info);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdSetFragmentShadingRateKHR(
   VkCommandBuffer commandBuffer, const VkExtent2D *pFragmentSize,
   const VkFragmentShadingRateCombinerOpKHR combinerOps[2])
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   vk_common_CmdSetFragmentShadingRateKHR(commandBuffer, pFragmentSize, combinerOps);
   /* Quad-group fragment shaders combine the rate from the root table. */
   uint32_t lw = pFragmentSize->width >= 4 ? 2 : pFragmentSize->width >= 2 ? 1 : 0;
   uint32_t lh = pFragmentSize->height >= 4 ? 2 : pFragmentSize->height >= 2 ? 1 : 0;
   uint32_t state = (lw << 2) | lh | (combinerOps[0] << 4) | (combinerOps[1] << 8);
   struct kk_descriptor_state *desc = &cmd->state.gfx.descriptors;
   if (desc->root.fsr.state != state) {
      desc->root.fsr.state = state;
      desc->root_dirty = true;
   }
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindIndexBuffer3KHR(VkCommandBuffer commandBuffer,
                          const VkBindIndexBuffer3InfoKHR *pInfo)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   cmd->state.gfx.index.gpu.addr = pInfo->addressRange.address;
   cmd->state.gfx.index.gpu.range = pInfo->addressRange.size;
   cmd->state.gfx.index.bytes_per_index =
      vk_index_type_to_bytes(pInfo->indexType);
   vk_cmd_set_index_buffer_type(&cmd->vk, pInfo->indexType);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindVertexBuffers3KHR(VkCommandBuffer commandBuffer,
                            uint32_t firstBinding, uint32_t bindingCount,
                            const VkBindVertexBuffer3InfoKHR *pBindingInfos)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   vk_cmd_set_vertex_binding_strides2(&cmd->vk, firstBinding, bindingCount,
                                      pBindingInfos);

   for (uint32_t i = 0; i < bindingCount; i++) {
      const VkStridedDeviceAddressRangeKHR range =
         pBindingInfos[i].addressRange;
      struct kk_addr_range *vb_range =
         &cmd->state.gfx.vb.addr_range[firstBinding + i];
      vb_range->addr = range.address;
      vb_range->range = range.size;
      cmd->state.gfx.dirty |= KK_DIRTY_VB;
   }
}

/* Metal places a line endpoint that lies exactly on a pixel row boundary in
 * the row above it, so a line drawn downward starts one row early and leaves
 * out its last row. Default lines are non-strict and may deviate by one pixel
 * (the Bresenham rules). Moving native default-mode lines one sub-pixel step
 * down puts such an endpoint in the row below: a downward line covers its
 * start through end rows, and an upward line takes the permitted one-pixel
 * deviation. On the sub-pixel grid no other position changes rows.
 */
static void
kk_flush_vp_state(struct kk_cmd_buffer *cmd, bool default_lines)
{
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   /* We always need at least 1 viewport for the hardware. With rasterizer
    * discard the app may not supply any, but we can just program garbage.
    */
   unsigned count = MAX2(dyn->vp.scissor_count, 1);

   /* Need to clamp scissor rectangles to render area, otherwise Metal doesn't
    * like it */
   struct mtl_scissor_rect rects[KK_MAX_VIEWPORTS] = {0};
   kk_render_scissors(&dyn->vp, &cmd->state.gfx.render.area, rects);

   mtl_render_encoder *encoder = cs_get_render(cmd);
   mtl_set_scissor_rects(encoder, rects, count);

   count = MAX2(dyn->vp.viewport_count, 1);

   struct mtl_viewport viewports[KK_MAX_VIEWPORTS] = {0};

   /* NDC in Metal is pointing downwards. Vulkan is pointing upwards. Account
    * for that here */
   for (uint32_t i = 0; i < dyn->vp.viewport_count; i++) {
      const VkViewport *vp = &dyn->vp.viewports[i];

      viewports[i].originX = vp->x;
      viewports[i].originY = vp->y + vp->height +
         (default_lines ? 1.0 / (1u << KK_SUBPIXEL_PRECISION_BITS) : 0.0);
      viewports[i].width = vp->width;
      viewports[i].height = -vp->height;

      if (dyn->rs.depth_clamp_enable ||
          vk_rasterization_state_depth_clip_enable(&dyn->rs)) {
         viewports[i].znear = vp->minDepth;
         viewports[i].zfar = vp->maxDepth;
      } else {
         /* When clamp and clip are disabled, we set the viewport Z range to
          * [0, 1] to disable hardware clamping. The viewport transform will be
          * applied in shader */
         viewports[i].znear = 0.f;
         viewports[i].zfar = 1.f;
      }
   }

   if (cmd->state.gfx.polygon_points) {
      const struct kk_rendering_state *render = &cmd->state.gfx.render;
      double width = MAX2(render->area.offset.x + render->area.extent.width, 1);
      double height = MAX2(render->area.offset.y + render->area.extent.height, 1);
      for (unsigned i = 0; i < count; ++i) {
         viewports[i] = (struct mtl_viewport){
            .originX = 0, .originY = height, .width = width, .height = -height,
            .znear = dyn->rs.depth_clamp_enable &&
               dyn->rs.polygon_mode != VK_POLYGON_MODE_POINT ? dyn->vp.viewports[i].minDepth : 0,
            .zfar = dyn->rs.depth_clamp_enable &&
               dyn->rs.polygon_mode != VK_POLYGON_MODE_POINT ? dyn->vp.viewports[i].maxDepth : 1,
         };
      }
   }
   mtl_set_viewports(encoder, viewports, count);
}

static inline uint32_t
kk_calculate_vbo_clamp(uint64_t vbuf, uint64_t sink, enum pipe_format format,
                       uint32_t size_B, uint32_t stride_B, uint32_t offset_B,
                       uint64_t *vbuf_out)
{
   unsigned elsize_B = util_format_get_blocksize(format);
   unsigned subtracted_B = offset_B + elsize_B;

   /* If at least one index is valid, determine the max. Otherwise, direct reads
    * to zero.
    */
   if (size_B >= subtracted_B) {
      *vbuf_out = vbuf + offset_B;

      /* If stride is zero, do not clamp, everything is valid. */
      if (stride_B)
         return ((size_B - subtracted_B) / stride_B);
      else
         return UINT32_MAX;
   } else {
      *vbuf_out = sink;
      return 0;
   }
}

/* The GS output primitive if one is bound, otherwise the input topology */
/* All polygon representations share this selection, including a native
 * passthrough when the original pipeline needs no tessellation, GS or XFB. */
static bool
kk_polygon_expansion_active(struct kk_cmd_buffer *cmd)
{
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   if (!vs || !vs->info.polygon_point) return false;
   if (cmd->state.gfx.poly_retained.consume ||
       (vs->info.poly_layer_replay &&
        (cmd->state.gfx.poly_replay_active ||
         (cmd->state.gfx.render.layered_bc && cmd->state.gfx.shader == vs))))
      return true;
   if (vs->info.polygon.cull_count) return true;
   bool controlled_bias = kk_depth_bias_control_active(
      vs->info.depth_bias_control, &dyn->rs);
   bool lines = vs->info.polygon.primitive_vertices == 2 ||
      dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE;
   return (vs->info.conservative && dyn->rs.conservative_mode ==
      VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT) ||
      dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias ||
      (lines && (dyn->rs.line.width != 1.0f ||
                 dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR ||
                 dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
                 dyn->rs.line.stipple.enable));
}

/* Synthetic XFB passthrough stages are retained by the VS pipeline. */
static struct kk_shader *
kk_geometry_shader(struct kk_cmd_buffer *cmd)
{
   struct kk_shader *gs = cmd->state.shaders[MESA_SHADER_GEOMETRY];
   struct kk_shader *vs = cmd->state.gfx.shader;
   if (!gs && vs && vs->info.polygon_native && !kk_polygon_expansion_active(cmd) &&
       !(cmd->state.gfx.layer_replay.bind && vs->info.poly_layer_replay))
      return NULL;
   return gs ? gs : vs && vs->info.has_geometry ? vs : NULL;
}

static enum mesa_prim
kk_rast_prim(struct kk_cmd_buffer *cmd)
{
   struct kk_shader *shader = cmd->state.gfx.shader;
   if (shader && shader->info.stage == MESA_SHADER_MESH)
      return shader->mesh->info.primitive;
   struct kk_shader *gs = kk_geometry_shader(cmd);
   if (gs)
      return gs->info.geometry.mode;

   return vk_topology_to_mesa(
      cmd->vk.dynamic_graphics_state.ia.primitive_topology);
}

static void
set_empty_scissor(mtl_render_encoder *enc)
{
   struct mtl_scissor_rect rect = {.x = 0u, .y = 0u, .width = 0u, .height = 0u};
   mtl_set_scissor_rects(enc, &rect, 1);
}

#define IS_DIRTY(bit) BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_##bit)
#define IS_SHADER_DIRTY(bit)                                                   \
   (cmd->state.dirty_shaders & BITFIELD_BIT(MESA_SHADER_##bit))

static bool
kk_flush_sample_locations(struct kk_cmd_buffer *cmd)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_rendering_state *render = &gfx->render;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;

   bool needs_update = false;

   /* Determine if the user-provided custom sample locations have changed */
   if (IS_DIRTY(MS_SAMPLE_LOCATIONS_ENABLE) || IS_DIRTY(MS_SAMPLE_LOCATIONS) ||
       IS_DIRTY(MS_RASTERIZATION_SAMPLES)) {
      needs_update |=
         render->sample_locations_enable != dyn->ms.sample_locations_enable;
      render->sample_locations_enable = dyn->ms.sample_locations_enable;

      if (render->sample_locations_enable) {
         struct vk_sample_locations_state *sample_locations =
            dyn->ms.sample_locations;

         uint32_t count = sample_locations->per_pixel *
                          sample_locations->grid_size.width *
                          sample_locations->grid_size.height;
         needs_update |= render->sample_locations_count != count;
         needs_update |= memcmp(render->sample_locations,
                                dyn->ms.sample_locations->locations,
                                count * sizeof(VkSampleLocationEXT)) != 0;

         render->sample_locations_count = count;
         typed_memcpy(render->sample_locations,
                      dyn->ms.sample_locations->locations, count);
      }
   }

   /* Determine if we have switched to or from multisampled bresenham lines */
   if (IS_DIRTY(IA_PRIMITIVE_TOPOLOGY) || IS_DIRTY(RS_LINE_MODE) ||
       IS_DIRTY(MS_RASTERIZATION_SAMPLES)) {
      bool was_ms_bresenham_lines = render->ms_bresenham_lines;
      render->ms_bresenham_lines =
         u_reduced_prim(dyn->ia.primitive_topology) == MESA_PRIM_LINES &&
         dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM &&
         render->samples > 1;

      needs_update |= was_ms_bresenham_lines != render->ms_bresenham_lines;
   }

   if (needs_update) {
      if (render->sample_locations_enable) {
         /* Prioritize user-provided sample locations */
         struct mtl_sample_position sample_positions[KK_MAX_SAMPLES];
         for (uint32_t i = 0; i < KK_MAX_SAMPLES; i++) {
            VkSampleLocationEXT sl = render->sample_locations[i];
            sample_positions[i] = (struct mtl_sample_position){
               /* Metal asserts if values are out of range. Vulkan spec says
                * values are clamped, and dynamic state CTS tests hit this */
               .x = CLAMP(sl.x, KK_MIN_SAMPLE_LOCATION, KK_MAX_SAMPLE_LOCATION),
               .y = CLAMP(sl.y, KK_MIN_SAMPLE_LOCATION, KK_MAX_SAMPLE_LOCATION),
            };
         }

         mtl_render_pass_descriptor_set_sample_positions(
            gfx->render_pass_descriptor, sample_positions,
            render->sample_locations_count);
      } else if (render->ms_bresenham_lines) {
         /* For default sample locations with bresenham lines, set all to center
          * to provide correct rasterization */
         static const struct mtl_sample_position center = {0.5f, 0.5f};
         static const struct mtl_sample_position center_all[KK_MAX_SAMPLES] = {
            center, center, center, center, center, center, center, center};
         mtl_render_pass_descriptor_set_sample_positions(
            gfx->render_pass_descriptor, center_all, gfx->render.samples);
      } else {
         /* If custom sample locations are not needed, reset them */
         mtl_render_pass_descriptor_set_sample_positions(
            gfx->render_pass_descriptor, NULL, 0);
      }
   }

   return needs_update;
}

static void
kk_flush_render_pass(struct kk_cmd_buffer *cmd)
{
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   cmd->state.gfx.pipeline_sample_count = MAX2(dyn->ms.rasterization_samples, 1);
   bool needs_restart = false;
   /* Metal fixes the default sample count when an encoder starts. Vulkan may
    * change it between attachmentless draws in the same rendering scope. */
   if (render->attachmentless &&
       render->samples != cmd->state.gfx.pipeline_sample_count) {
      render->samples = cmd->state.gfx.pipeline_sample_count;
      mtl_render_pass_descriptor_set_default_raster_sample_count(
         cmd->state.gfx.render_pass_descriptor, render->samples);
      needs_restart = true;
   }
   needs_restart |= kk_flush_sample_locations(cmd);
   bool color_attachment_map_changed = false;

   if (IS_DIRTY(COLOR_ATTACHMENT_MAP)) {
      if (memcmp(render->color_map, dyn->cal.color_map,
                 render->color_att_count * sizeof(render->color_map[0])) != 0) {
         assert(cmd->state.gfx.render_pass_descriptor);

         /* Apply the store ops before the new color map is stored. */
         kk_apply_attachment_store_ops(cmd, true);

         kk_set_color_attachments(
            cmd, cmd->state.gfx.render_pass_descriptor, render, dyn, true,
            cmd->state.gfx.render.rendering_to_whole_framebuffer, true);
         needs_restart = true;
         color_attachment_map_changed = true;
      }
   }
   /* If render pass state changes and the pass is currently active, end the
    * current encoder and prepare to restart it */
   bool active_render = cmd->metal.render != NULL;
   if (needs_restart && active_render) {
      if (!color_attachment_map_changed) {
         /* Sample locations changed and color attachment map didn't. */
         kk_apply_attachment_store_ops(cmd, true);
      }

      cs_end(cmd);
      kk_cmd_buffer_invalidate_gfx_encoder(cmd);
      cmd->state.gfx.need_to_start_render_pass = true;
   }
}

/* Select the quad variant for shared multisample effects, or while the
 * combined rate can exceed 1x1. */
static unsigned
kk_fsr_pipeline_kind(const struct kk_cmd_buffer *cmd, const struct kk_shader *vs)
{
   if (!vs->info.vs.fsr_coarse)
      return KK_PIPELINE_MAIN;
   /* Slot exchange is required at 1x1 too: samples of one pixel must share
    * the application's side effects and published outputs. */
   if (vs->info.vs.fsr_coarse_slots)
      return KK_PIPELINE_FSR_COARSE;
   const struct vk_fragment_shading_rate_state *fsr = &cmd->vk.dynamic_graphics_state.fsr;
   bool fine = fsr->fragment_size.width <= 1 && fsr->fragment_size.height <= 1;
   fine = kk_fsr_combiner_is_fine(fine, !vs->info.vs.fsr_primitive_rate, fsr->combiner_ops[0]);
   fine = kk_fsr_combiner_is_fine(fine, !cmd->state.gfx.descriptors.root.fsr.attachment,
                                  fsr->combiner_ops[1]);
   return fine ? KK_PIPELINE_MAIN : KK_PIPELINE_FSR_COARSE;
}

/* Only the coarse variant reads the rate state after the push constants. */
static void
kk_fsr_variant_bound(struct kk_cmd_buffer *cmd, const struct kk_shader *vs, bool coarse)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   if (!vs->info.vs.fsr_coarse && !gfx->fsr_coarse)
      return;
   struct kk_descriptor_state *desc = &gfx->descriptors;
   uint32_t push_size = coarse ? KK_FSR_PUSH_SIZE : vs->info.push_size;
   desc->root_dirty |= push_size > desc->push_size;
   desc->push_size = push_size;
   gfx->fsr_coarse = coarse;
}

static void
kk_flush_pipeline(struct kk_cmd_buffer *cmd)
{
   struct kk_device *device = kk_cmd_buffer_device(cmd);
   mtl_render_encoder *enc = cs_get_render(cmd);
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;

   bool bind_dynamic_ds = false;

   /* Depth/stencil state may be dynamic, handle it as part of the pipeline. */
   if (cmd->state.gfx.is_depth_stencil_dynamic &&
       (gfx->encoder_dirty ||
        IS_DIRTY(DS_DEPTH_TEST_ENABLE) | IS_DIRTY(DS_DEPTH_WRITE_ENABLE) |
        IS_DIRTY(DS_DEPTH_COMPARE_OP) | IS_DIRTY(DS_STENCIL_TEST_ENABLE) |
        IS_DIRTY(DS_STENCIL_OP) | IS_DIRTY(DS_STENCIL_COMPARE_MASK) |
        IS_DIRTY(DS_STENCIL_WRITE_MASK) || (IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(MESH)))) {
      struct kk_shader *vs = cmd->state.gfx.shader;
      bool has_depth = gfx->render.depth_att.iview &&
         vs->info.vs.d_format != MTL_PIXEL_FORMAT_INVALID;
      bool has_stencil = gfx->render.stencil_att.iview &&
         vs->info.vs.s_format != MTL_PIXEL_FORMAT_INVALID;
      gfx->depth_stencil_state = kk_update_dynamic_ds_state(
         device, &dyn->ds, has_depth, has_stencil,
         &gfx->dynamic_ds_key, gfx->depth_stencil_state);
      bind_dynamic_ds = true;
   }

   if ((gfx->shader->info.uses_sampler_emulation &&
        kk_attachment_sampler_emulation_changed(cmd, gfx->shader)) ||
       (gfx->shader->info.vs.fsr_coarse && IS_DIRTY(FSR)) ||
       gfx->encoder_dirty || (gfx->dirty & KK_DIRTY_FRAGMENT_STATISTICS) ||
       (IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(MESH)) || IS_DIRTY(RS_POLYGON_MODE) ||
       IS_DIRTY(CB_COLOR_WRITE_ENABLES) || IS_DIRTY(COLOR_ATTACHMENT_MAP) ||
       IS_DIRTY(MS_RASTERIZATION_SAMPLES) || IS_DIRTY(RS_LINE_MODE)) {
      struct kk_shader *vs = cmd->state.gfx.shader;
      unsigned kind = gfx->conservative ? 2 :
         (gfx->polygon_points || (gfx->layer_replay.bind && vs->info.poly_layer_replay));
      if (kind == KK_PIPELINE_MAIN)
         kind = kk_fsr_pipeline_kind(cmd, vs);
      kk_fsr_variant_bound(cmd, vs, kind == KK_PIPELINE_FSR_COARSE);
      mtl_render_pipeline_state *pipeline = kk_attachment_pipeline(cmd, vs, kind);
      if (!pipeline) {
         if (bind_dynamic_ds)
            mtl_set_depth_stencil_state(enc, gfx->depth_stencil_state);
         return;
      }
      mtl_render_set_pipeline_state(enc, pipeline);
      if (vs->info.uses_sampler_emulation)
         gfx->sampler_emulation = kk_attachment_sampler_emulation_value(device, vs);
      mtl_depth_stencil_state *ds = kk_cmd_selected_depth_stencil(cmd, vs);
      if (ds || bind_dynamic_ds)
         mtl_set_depth_stencil_state(enc, ds);
   } else if (bind_dynamic_ds) {
      mtl_set_depth_stencil_state(enc, gfx->depth_stencil_state);
   }

   /* Merge tess info before GS construction since that depends on
    * gfx->tess.prim
    */
   if ((IS_SHADER_DIRTY(TESS_CTRL) || IS_SHADER_DIRTY(TESS_EVAL)) &&
       cmd->state.shaders[MESA_SHADER_TESS_CTRL]) {
      struct kk_shader *tesc = cmd->state.shaders[MESA_SHADER_TESS_CTRL];
      struct kk_shader *tese = cmd->state.shaders[MESA_SHADER_TESS_EVAL];

      gfx->tess.info =
         kk_tess_info_merge(tese->info.tess.info, tesc->info.tess.info);

      /* Determine primitive based on the merged state */
      if (gfx->tess.info.points) {
         gfx->tess.prim = MESA_PRIM_POINTS;
      } else if (gfx->tess.info.mode == TESS_PRIMITIVE_ISOLINES) {
         gfx->tess.prim = MESA_PRIM_LINES;
      } else {
         gfx->tess.prim = MESA_PRIM_TRIANGLES;
      }
   }
}

static void
kk_init_heap(const void *data)
{
   struct kk_cmd_buffer *cmd = (struct kk_cmd_buffer *)data;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);

   size_t size = KK_POLY_HEAP_SIZE;
   kk_alloc_bo(dev, &dev->vk.base, size, 0, &dev->heap);

   struct poly_heap *map = (struct poly_heap *)dev->heap->cpu;

   /* TODO_KOSMICKRISP Self-contained until we have rodata at the device. */
   *map = (struct poly_heap){
      .base = dev->heap->gpu + sizeof(struct poly_heap),
      .size = size - sizeof(struct poly_heap),
   };
}

static void
kk_init_fsr_slots(const void *data)
{
   struct kk_device *dev = (struct kk_device *)data;
   if (kk_alloc_bo(dev, &dev->vk.base, KK_FSR_SLOT_BYTES, 16, &dev->fsr_slots) == VK_SUCCESS) {
      memset(dev->fsr_slots->cpu, 0, dev->fsr_slots->size_B);
      kk_bo_set_label(dev->fsr_slots, "Fragment shading rate sample slots");
   }
}

static struct kk_bo *
kk_draw_heap(struct kk_cmd_buffer *cmd)
{
   if (cmd->interlock_heap_active)
      return kk_interlock_heap_bo(cmd->interlock_heap);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   util_call_once_data(&dev->heap_init_once, kk_init_heap, cmd);
   return dev->heap;
}

static uint64_t
kk_heap(struct kk_cmd_buffer *cmd)
{
   struct kk_bo *heap = kk_draw_heap(cmd);

   /* We need to free all allocations after each command buffer execution */
   bool *uses_heap = cmd->interlock_heap_active ?
      &cmd->interlock_heap_used : &cmd->uses_heap;
   if (!*uses_heap) {
      uint64_t addr = heap->gpu;

      /* Zeroing the allocated index frees everything. Force compute encoder
       * since this path is only taken by tessellation, GS and unroll, which
       * already force a compute encoder break */
      mtl_compute_encoder *encoder = cs_get_compute(cmd);
      if (cmd->state.gfx.generated_heap_floor) {
         libkk_dgc_reserve_heap(cmd, kk_grid_1d(1), true, addr, cmd->state.gfx.generated_heap_floor);
      } else {
         kk_cmd_write(cmd, (struct libkk_imm_write){addr + offsetof(struct poly_heap, bottom), 0});
      }

      /* Ensure heap is set to 0 before we allocate anything. */
      KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder, MTL_STAGE_DISPATCH,
                                       MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);

      *uses_heap = true;
   }

   return heap->gpu;
}

/* GPU-generated indices are allocated from the heap, past its header */
static struct kk_addr_range
kk_heap_indices(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   uint64_t heap = kk_heap(cmd);

   /* TODO_KOSMICKRISP Self-contained until we have rodata at the device. */
   return (struct kk_addr_range){
      .addr = heap + sizeof(struct poly_heap),
      .range = kk_draw_heap(cmd)->size_B - sizeof(struct poly_heap),
   };
}

enum kk_predicate_op : uint16_t {
   /* value > draw_id */
   KK_PREDICATE_GT_DRAW_ID,
   /* value == 0 */
   KK_PREDICATE_EQ_ZERO,
   /* value != 0 */
   KK_PREDICATE_NEQ_ZERO,
};

struct kk_draw_command {
   enum mesa_prim prim;
   /* Mask of stages that need per-draw data uploaded */
   uint32_t upload_mask;
   struct kk_addr_range index_buffer;
   uint32_t restart_index;
   uint8_t index_buffer_el_size_B;
   bool indirect;
   bool indexed;
   bool restart;
   bool flatshade_first;
   uint8_t pad_[3];
   uint32_t predicate_count;
   enum kk_predicate_op predicate_op[2];
   uint32_t draw_count;
   uint64_t predicate_addr[2];

   union {
      struct {
         uint64_t addr;
         uint32_t stride;
      } indirect_command;
      /* These arrays will be >1 when draw_count is >1 as this struct is
       * dynamically allocated. */
      VkDrawIndirectCommand draws[1];
      VkDrawIndexedIndirectCommand indexed_draws[1];
   };
};
static_assert(sizeof(struct kk_draw_command) == 88u, "Packed struct");

struct kk_draw_data {
   /* For non-indirect, 0 is vertex/index count, 1 instance count and 2 first
    * instance */
   struct kk_grid grid;
   struct {
      struct kk_addr_range gpu;
      uint32_t el_size_B;
   } index;
   uint32_t vertex_offset;
   enum mtl_primitive_type primitive_type;
};

static uint64_t
kk_upload_vertex_params(struct kk_cmd_buffer *cmd,
                        const struct kk_draw_data *data)
{
   /* Threadgroup size of indirect-local dispatches */
   const uint32_t wg_size[3] = {KK_POLY_WG_SIZE, 1, 1};

   struct poly_vertex_params params;
   poly_vertex_params_init(&params, 0, wg_size);

   /* XXX: We should deduplicate this logic */
   bool indirect = kk_grid_is_indirect(data->grid);

   if (!indirect)
      poly_vertex_params_set_draw(&params, data->grid.size.x,
                                  data->grid.size.y);

   if (data->index.el_size_B) {
      params.index_buffer = data->index.gpu.addr;
      params.index_buffer_range_el =
         data->index.gpu.range / data->index.el_size_B;
   }

   struct kk_shader *vs = cmd->state.gfx.shader;
   params.outputs = vs->info.outputs_written;

   if (!indirect) {
      uint32_t verts = data->grid.size.x, instances = data->grid.size.y;
      unsigned vb_size =
         poly_tcs_in_size(verts * instances, vs->info.outputs_written);

      /* Allocate if there are any outputs, or use the null sink to trap
       * reads if there aren't. Those reads are undefined but should not
       * fault. Affects:
       *
       *    dEQP-VK.pipeline.monolithic.no_position.explicit_declarations.basic.single_view.v0_g1
       */
      if (vb_size)
         params.output_buffer = kk_pool_alloc(cmd, vb_size, 4).gpu;
      else
         params.output_buffer = 0u;
   }

   cmd->state.gfx.per_draw_data.vertex_outputs = params.outputs;

   return kk_pool_upload(cmd, &params, sizeof(params), 8).gpu;
}

static uint64_t kk_pipeline_stat_addr(const struct kk_cmd_buffer *, VkQueryPipelineStatisticFlagBits);

static void
kk_upload_tess_params(struct kk_cmd_buffer *cmd, struct poly_tess_params *out,
                      const struct kk_draw_data *draw)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *tcs = cmd->state.shaders[MESA_SHADER_TESS_CTRL];

   enum poly_tess_partitioning partitioning =
      gfx->tess.info.spacing == TESS_SPACING_EQUAL
         ? POLY_TESS_PARTITIONING_INTEGER
      : gfx->tess.info.spacing == TESS_SPACING_FRACTIONAL_ODD
         ? POLY_TESS_PARTITIONING_FRACTIONAL_ODD
         : POLY_TESS_PARTITIONING_FRACTIONAL_EVEN;

   struct poly_tess_params args = {
      .heap = kk_heap(cmd),
      .tcs_stride_el = tcs->info.tess.tcs_output_stride / 4,
      .statistic = (cmd->state.pipeline_statistics_flags &
         VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT) ?
         kk_pool_upload(cmd, (uint64_t[1]){0}, sizeof(uint64_t), 8).gpu : 0u,
      .input_patch_size = dyn->ts.patch_control_points,
      .output_patch_size = tcs->info.tess.tcs_output_patch_size,
      .tcs_patch_constants = tcs->info.tess.tcs_nr_patch_outputs,
      .tcs_per_vertex_outputs = tcs->info.tess.tcs_per_vertex_outputs,
      .partitioning = partitioning,
      .points_mode = gfx->tess.info.points,
      .isolines = gfx->tess.info.mode == TESS_PRIMITIVE_ISOLINES,
   };

   if (!args.points_mode && gfx->tess.info.mode != TESS_PRIMITIVE_ISOLINES) {
      args.ccw = gfx->tess.info.ccw;
      args.ccw ^=
         dyn->ts.domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
   }

   uint32_t draw_stride_el = 5;
   size_t draw_stride_B = draw_stride_el * sizeof(uint32_t);

   /* heap is allocated by kk_heap */
   /* TODO_KOSMICKRISP Self-contained until we have rodata at the device. */
   args.patch_coord_buffer = kk_draw_heap(cmd)->gpu + sizeof(struct poly_heap);

   if (!kk_grid_is_indirect(draw->grid)) {
      unsigned in_patches = draw->grid.size.x / args.input_patch_size;
      unsigned unrolled_patches = in_patches * draw->grid.size.y;

      uint32_t alloc = 0;
      uint32_t tcs_out_offs = alloc;
      alloc += unrolled_patches * args.tcs_stride_el * sizeof(uint32_t);

      uint32_t patch_coord_offs = alloc;
      alloc += unrolled_patches * sizeof(uint32_t);

      uint32_t count_offs = alloc;
      alloc += unrolled_patches * sizeof(uint32_t);

      /* Single API draw */
      uint32_t draw_offs = alloc;
      alloc += draw_stride_B;

      struct kk_ptr ptr = kk_pool_alloc(cmd, alloc, 4);
      gfx->tess.out_draws_addr = ptr.gpu + draw_offs;
      uint64_t addr = ptr.gpu;
      args.tcs_buffer = addr + tcs_out_offs;
      args.patches_per_instance = in_patches;
      args.coord_allocs = addr + patch_coord_offs;
      args.nr_patches = unrolled_patches;
      args.out_draws = addr + draw_offs;
      args.counts = addr + count_offs;
   } else {
      /* Allocate 3x indirect global+local grids for VS/TCS/tess */
      uint32_t grid_stride = sizeof(struct kk_tess_grid);
      gfx->tess.indirect_ptr = kk_pool_alloc(cmd, grid_stride * 3, 4);

      struct kk_ptr ptr = kk_pool_alloc(cmd, draw_stride_B, 4);
      gfx->tess.out_draws_addr = ptr.gpu;
      args.out_draws = ptr.gpu;
   }

   memcpy(out, &args, sizeof(args));
}

static enum mesa_prim
kk_gs_in_prim(struct kk_cmd_buffer *cmd)
{
   if (cmd->state.shaders[MESA_SHADER_TESS_EVAL])
      return cmd->state.gfx.tess.prim;

   return vk_topology_to_mesa(
      cmd->vk.dynamic_graphics_state.ia.primitive_topology);
}

static uint64_t
kk_upload_geometry_params(struct kk_cmd_buffer *cmd,
                          const struct kk_draw_data *draw, bool restart)
{
   /* The tessellator output always feeds the GS indirectly */
   bool indirect = kk_grid_is_indirect(draw->grid) ||
                   cmd->state.shaders[MESA_SHADER_TESS_EVAL];
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *gs = kk_geometry_shader(cmd);
   enum poly_gs_shape shape = gs->info.geometry.shape;
   uint32_t max_indices = gs->info.geometry.max_indices;

   enum mesa_prim mode = kk_gs_in_prim(cmd);
   if (restart)
      mode = u_decomposed_prim(mode);

   const uint32_t wg_size[3] = {KK_POLY_WG_SIZE, 1, 1};

   struct poly_geometry_params params;
   poly_geometry_params_init(&params, mode, wg_size);
   const struct poly_gs_info *info = &gs->info.geometry;
   params.count_buffer_stride = info->count_words * sizeof(uint32_t);
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   params.flat_outputs = fs ? fs->info.fs.flat_outputs : 0;
   bool statistics = gfx->shader->info.geometry_statistics &&
      (cmd->state.pipeline_statistics_flags & KK_GEOMETRY_QUERY_BITS);
   /* Polygon capture writes dynamic counts even without queries. The
    * ordinary GS path only consumes them for XFB or actual reports. */
   bool count_storage = gfx->polygon_points || cmd->interlock_heap_active ||
      info->xfb || statistics || gfx->geometry_query_mask;
   uint32_t statistics_bytes = statistics ?
      KK_GEOMETRY_STAT_COUNT * sizeof(uint32_t) : 0;
   uint32_t count_bytes = count_storage && info->count_words && !info->prefix_sum ?
      info->count_words * sizeof(uint32_t) : 0;
   uint64_t scratch = 0;
   if (statistics_bytes || count_bytes) {
      struct kk_ptr storage = kk_pool_alloc(cmd, statistics_bytes + count_bytes, 8);
      if (unlikely(!storage.cpu))
         return 0;
      memset(storage.cpu, 0, statistics_bytes + count_bytes);
      scratch = storage.gpu;
   }
   params.prims_generated_counter_secondary = gfx->xfb_generated_counters;
   for (unsigned i = 0; i < POLY_MAX_VERTEX_STREAMS; ++i) {
      params.prims_generated_counter[i] = gfx->primitives_query[i];
      params.xfb_prims_generated_counter[i] = gfx->xfb_query[i];
      if (gfx->xfb_enabled) {
         params.xfb_base_original[i] = gfx->xfb[i].addr;
         params.xfb_size[i] = gfx->xfb[i].range;
         params.xfb_offs_ptrs[i] = gfx->xfb_offsets + i * sizeof(uint32_t);
      }
   }
   if (statistics) {
      for (unsigned i = 0; i < KK_GEOMETRY_STAT_COUNT; ++i)
         gfx->per_draw_data.geometry_statistics[i] =
            scratch + i * sizeof(uint32_t);
   }
   if (count_bytes)
      params.count_buffer = scratch + statistics_bytes;

   if (indirect) {
      /* The indirect setup kernel writes the draw and allocates from the
       * heap, including the index buffer */
      if (shape == POLY_GS_SHAPE_DYNAMIC_INDEXED)
         gfx->gs.index = kk_heap_indices(cmd);
   } else {
      poly_geometry_params_set_draw(&params, mode, shape, max_indices,
                                    draw->grid.size.x, draw->grid.size.y);

      if (info->prefix_sum)
         params.count_buffer = kk_pool_alloc(cmd,
            params.input_primitives * params.count_buffer_stride, 4).gpu;
      gfx->gs.index_count = params.draw.index_count;
      gfx->gs.instance_count = params.draw.instance_count;

      if (shape == POLY_GS_SHAPE_DYNAMIC_INDEXED) {
         uint32_t size_B = gfx->gs.index_count * sizeof(uint32_t);
         params.output_index_buffer = kk_pool_alloc(cmd, size_B, 4).gpu;
         gfx->gs.index.addr = params.output_index_buffer;
         gfx->gs.index.range = size_B;
      }
   }

   if (shape == POLY_GS_SHAPE_STATIC_INDEXED) {
      /* Metal has no 8-bit indices and restarts 16-bit ones at 0xFFFF */
      uint16_t indices[ARRAY_SIZE(gs->info.geometry.topology)];
      for (uint32_t i = 0u; i < max_indices; ++i) {
         uint8_t index = gs->info.geometry.topology[i];
         indices[i] = index == UINT8_MAX ? UINT16_MAX : index;
      }

      uint32_t size_B = max_indices * sizeof(uint16_t);
      gfx->gs.index.addr = kk_pool_upload(cmd, indices, size_B, 4).gpu;
      gfx->gs.index.range = size_B;
   }

   if (gfx->polygon_points || cmd->interlock_heap_active) {
      const struct poly_gs_capture_info *capture = &gs->info.polygon;
      const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
      struct poly_geometry_capture point = {
         .outputs = capture->outputs,
         .float_outputs = capture->float_outputs,
         .noperspective_outputs = fs ? fs->info.fs.noperspective_outputs : 0,
         .vertex_stride = util_bitcount64(capture->outputs) * 16,
         .vertices_out = capture->vertices_out,
         .primitive_vertices = capture->primitive_vertices,
         .line_expand = capture->primitive_vertices == 2 ||
            (capture->primitive_vertices == 3 && dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE),
         .line_mode = dyn->rs.line.mode,
         .line_stipple_flags = dyn->rs.line.stipple.enable ?
            KK_LINE_STIPPLE_ENABLED |
            ((fs && fs->info.fs.line_geometry_stipple &&
              (capture->primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE)) ?
               KK_LINE_STIPPLE_GEOMETRY : 0) : 0,
         .line_stipple_factor = dyn->rs.line.stipple.factor,
         .line_stipple_pattern = dyn->rs.line.stipple.pattern,
         .line_strip = !cmd->state.shaders[MESA_SHADER_VERTEX]->info.api_geometry &&
            !cmd->state.shaders[MESA_SHADER_TESS_EVAL] &&
            (dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
             dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY),
         .line_width = kk_polygon_line_width(dyn->rs.line.width),
         .streams = capture->streams,
         .clip_count = capture->clip_count,
         .cull_count = capture->cull_count,
         .cull_mode = capture->primitive_vertices <= 2 ? 0 : dyn->rs.cull_mode,
         .front_ccw = dyn->rs.front_face != VK_FRONT_FACE_CLOCKWISE,
         .depth_clip = vk_rasterization_state_depth_clip_enable(&dyn->rs),
         .depth_clamp = dyn->rs.depth_clamp_enable,
         .negative_one_to_one = dyn->vp.depth_clip_negative_one_to_one,
         .provoking_last = dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT,
         .rasterization_stream = dyn->rs.rasterization_stream,
         .depth_bias_constant = dyn->rs.depth_bias.constant_factor,
         .depth_bias_slope = dyn->rs.depth_bias.slope_factor,
         .depth_bias_clamp = dyn->rs.depth_bias.clamp,
         .depth_bias_enable = dyn->rs.depth_bias.enable,
         .depth_bias_representation = dyn->rs.depth_bias.representation,
         .polygon_mode = capture->primitive_vertices == 1 ? VK_POLYGON_MODE_FILL : dyn->rs.polygon_mode,
         .depth_mantissa_bits = 23, .depth_is_float = true,
         .render_size = {
            MAX2(gfx->render.area.offset.x + gfx->render.area.extent.width, 1),
            MAX2(gfx->render.area.offset.y + gfx->render.area.extent.height, 1)},
      };
      if (cmd->state.shaders[MESA_SHADER_TESS_EVAL]) {
         point.front_ccw ^= gfx->tess.info.ccw;
         point.front_ccw ^= dyn->ts.domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
      }
      VkFormat depth = gfx->render.depth_att.vk_format;
      if (depth == VK_FORMAT_D16_UNORM || depth == VK_FORMAT_D16_UNORM_S8_UINT) {
         point.depth_mantissa_bits = 16; point.depth_is_float = false;
      } else if (depth == VK_FORMAT_X8_D24_UNORM_PACK32 || depth == VK_FORMAT_D24_UNORM_S8_UINT) {
         point.depth_mantissa_bits = 24; point.depth_is_float = false;
      }
      for (unsigned i = 0; i < dyn->vp.viewport_count; ++i) {
         const VkViewport *vp = &dyn->vp.viewports[i];
         point.viewport[i][0] = vp->x; point.viewport[i][1] = vp->y;
         point.viewport[i][2] = vp->width; point.viewport[i][3] = vp->height;
         point.viewport[i][4] = vp->minDepth; point.viewport[i][5] = vp->maxDepth;
      }
      if (gfx->conservative) {
         struct kk_conservative_params conservative = {
            .polygon = point,
            .extra_size = dyn->rs.extra_primitive_overestimation_size,
            .samples = MAX2(dyn->ms.rasterization_samples, 1),
            .sample_mask = dyn->ms.sample_mask,
            .canonical_locations = cmd->state.gfx.shader->info.conservative_canonical_outputs,
            .inner_coverage = cmd->state.gfx.shader->info.conservative_full_coverage,
            .canonical_provoking_components = {
               cmd->state.gfx.shader->info.conservative_canonical_components[0],
               cmd->state.gfx.shader->info.conservative_canonical_components[1],
            },
         };
         params.capture = gfx->polygon_capture =
            kk_pool_upload(cmd, &conservative, sizeof(conservative), 8).gpu;
      } else {
         if ((point.line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY) && point.line_mode != 2) {
         const struct vk_sample_locations_state *locations = dyn->ms.sample_locations_enable ?
            dyn->ms.sample_locations : vk_standard_sample_locations_state(MAX2(dyn->ms.rasterization_samples, 1u));
         uint32_t samples = locations->per_pixel;
         uint32_t header[2] = {samples, 0};
         struct kk_ptr capture_data = kk_pool_alloc(cmd,
            sizeof(point) + sizeof(header) + samples * sizeof(VkSampleLocationEXT), 8);
         memcpy(capture_data.cpu, &point, sizeof(point));
         memcpy((char *)capture_data.cpu + sizeof(point), header, sizeof(header));
         VkSampleLocationEXT *output_locations = (void *)((char *)capture_data.cpu +
            sizeof(point) + sizeof(header));
         for (uint32_t i = 0; i < samples; ++i) {
            output_locations[i].x = CLAMP(locations->locations[i].x,
               KK_MIN_SAMPLE_LOCATION, KK_MAX_SAMPLE_LOCATION);
            output_locations[i].y = CLAMP(locations->locations[i].y,
               KK_MIN_SAMPLE_LOCATION, KK_MAX_SAMPLE_LOCATION);
         }
         params.capture = gfx->polygon_capture = capture_data.gpu;
      } else {
         if (cmd->interlock_heap_active) {
         unsigned envelope_size = gs->info.depth_bounds_emulation ?
            sizeof(struct kk_depth_bounds_envelope) : sizeof(struct kk_interlock_envelope);
         gfx->interlock_capture = kk_pool_alloc(cmd, envelope_size, 8);
         if (gfx->interlock_capture.cpu) {
            memset(gfx->interlock_capture.cpu, 0, envelope_size);
            struct kk_interlock_envelope *env = gfx->interlock_capture.cpu;
            env->geometry = point;
            env->view_mask = gfx->render.view_mask;
            env->source_view = gfx->interlock_source_view;
            env->source_view_ordinal = gfx->interlock_source_view_ordinal;
         }
         params.capture = gfx->polygon_capture = gfx->interlock_capture.gpu;
      } else {
         params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;
      }
      }
      }
   }
   return kk_pool_upload(cmd, &params, sizeof(params), 8).gpu;
}

#include "kk_vertex_input.h"

static void
kk_flush_dynamic_state(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_descriptor_state *desc = &gfx->descriptors;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   mtl_render_encoder *enc = cs_get_render(cmd);
   bool flush_scissors = gfx->encoder_dirty;

   if (IS_DIRTY(VI_BINDING_STRIDES)) {
      u_foreach_bit(ndx, dyn->vi->bindings_valid) {
         desc->root.draw.buffer_strides[ndx] = dyn->vi_binding_strides[ndx];
      }
      desc->root_dirty = true;
   }

   if (IS_DIRTY(RS_RASTERIZER_DISCARD_ENABLE) ||
       IS_DIRTY(RS_RASTERIZATION_STREAM) || (IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(MESH)) ||
       IS_SHADER_DIRTY(GEOMETRY)) {
      const struct kk_shader *gs = kk_geometry_shader(cmd);
      gfx->rasterizer_discard = dyn->rs.rasterizer_discard_enable ||
         (dyn->rs.rasterization_stream != 0 &&
          !(gs && gs->info.geometry.multistream));
      gfx->per_draw_data.rasterization_stream = dyn->rs.rasterization_stream;
      flush_scissors = true;
   }
   if (flush_scissors && gfx->rasterizer_discard)
      set_empty_scissor(enc);

   if (IS_DIRTY(RS_CULL_MODE) || IS_DIRTY(IA_PRIMITIVE_TOPOLOGY) ||
       IS_SHADER_DIRTY(GEOMETRY)) {
      /* Only disable rendering if primitive type is culled. */
      gfx->is_cull_front_and_back =
         dyn->rs.cull_mode == VK_CULL_MODE_FRONT_AND_BACK &&
         u_reduced_prim(kk_rast_prim(cmd)) == MESA_PRIM_TRIANGLES;
      flush_scissors = true;
   }
   if (gfx->encoder_dirty || IS_DIRTY(RS_CULL_MODE) ||
       IS_DIRTY(IA_PRIMITIVE_TOPOLOGY) || IS_SHADER_DIRTY(GEOMETRY)) {
      if (gfx->is_cull_front_and_back) {
         set_empty_scissor(enc);
      } else {
         mtl_set_cull_mode(enc,
                           gfx->polygon_points ? MTL_CULL_MODE_NONE : vk_front_face_to_mtl_cull_mode(dyn->rs.cull_mode));
         flush_scissors = true;
      }
   }

   /* We enable raster discard by setting scissor to size (0, 0) */
   if (!(gfx->rasterizer_discard || gfx->is_cull_front_and_back) &&
       (flush_scissors || IS_DIRTY(VP_VIEWPORT_COUNT) || IS_DIRTY(VP_VIEWPORTS) ||
        IS_DIRTY(VP_SCISSOR_COUNT) || IS_DIRTY(VP_SCISSORS) ||
        IS_DIRTY(RS_DEPTH_CLAMP_ENABLE) || IS_DIRTY(RS_DEPTH_CLIP_ENABLE) ||
        IS_DIRTY(RS_LINE_MODE)))
      kk_flush_vp_state(cmd, dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_DEFAULT &&
                             u_reduced_prim(kk_rast_prim(cmd)) == MESA_PRIM_LINES);

   if (IS_DIRTY(VP_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE)) {
      desc->root.draw.clip_z_coeff =
         dyn->vp.depth_clip_negative_one_to_one ? 0.5f : 0.0f;
      desc->root_dirty = true;
   }

   if (gfx->encoder_dirty || IS_DIRTY(RS_FRONT_FACE) || IS_DIRTY(TS_DOMAIN_ORIGIN) ||
       IS_SHADER_DIRTY(TESS_CTRL) || IS_SHADER_DIRTY(TESS_EVAL)) {
      bool front_face_ccw = dyn->rs.front_face != VK_FRONT_FACE_CLOCKWISE;
      if (cmd->state.shaders[MESA_SHADER_TESS_EVAL]) {
         front_face_ccw ^= gfx->tess.info.ccw;
         front_face_ccw ^=
            dyn->ts.domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
      }
      mtl_set_front_face_winding(enc, front_face_ccw
                                         ? MTL_WINDING_COUNTER_CLOCKWISE
                                         : MTL_WINDING_CLOCKWISE);
   }

   if (gfx->encoder_dirty || IS_DIRTY(RS_POLYGON_MODE))
      mtl_set_triangle_fill_lines(enc, !gfx->polygon_points && dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE);

   if (gfx->encoder_dirty || IS_DIRTY(RS_DEPTH_BIAS_FACTORS) ||
       IS_DIRTY(RS_DEPTH_BIAS_ENABLE)) {
      if (dyn->rs.depth_bias.enable && !gfx->polygon_points)
         mtl_set_depth_bias(enc, dyn->rs.depth_bias.constant_factor,
                            dyn->rs.depth_bias.slope_factor,
                            dyn->rs.depth_bias.clamp);
      else
         mtl_set_depth_bias(enc, 0.0f, 0.0f, 0.0f);
   }

   if (gfx->encoder_dirty || IS_DIRTY(RS_DEPTH_CLAMP_ENABLE) ||
       IS_DIRTY(RS_DEPTH_CLIP_ENABLE) || IS_DIRTY(VP_VIEWPORT_COUNT) ||
       IS_DIRTY(VP_VIEWPORTS)) {
      /* Mapping of Vulkan clamp/clip combinations to Metal:
       * - Clamp Off, Clip Off:
       *     Use Metal's clamp mode with the viewport depth range set to [0, 1],
       *     effectively disabling both clip and clamp. Emulate the viewport Z
       *     transform in the vertex shader.
       * - Clamp Off, Clip On:
       *     Exact match to Metal's clip mode.
       * - Clamp On, Clip Off:
       *     Exact match to Metal's clamp mode.
       * - Clamp On, Clip On:
       *     Use Metal's clip mode, and emulate clamp in the fragment shader.
       */
      bool clamp = dyn->rs.depth_clamp_enable;
      bool clip = vk_rasterization_state_depth_clip_enable(&dyn->rs);

      enum mtl_depth_clip_mode mode =
         clip && !gfx->polygon_points ? MTL_DEPTH_CLIP_MODE_CLIP : MTL_DEPTH_CLIP_MODE_CLAMP;
      mtl_set_depth_clip_mode(enc, mode);
   }

   if (IS_DIRTY(RS_DEPTH_CLAMP_ENABLE) || IS_DIRTY(RS_DEPTH_CLIP_ENABLE) ||
       IS_DIRTY(VP_VIEWPORT_COUNT) || IS_DIRTY(VP_VIEWPORTS)) {
      bool clamp = dyn->rs.depth_clamp_enable;
      bool clip = vk_rasterization_state_depth_clip_enable(&dyn->rs);
      desc->root.draw.emulate_depth_clamp = clamp && clip;
      desc->root.draw.emulate_viewport_z = !clamp && !clip;
      if (desc->root.draw.emulate_depth_clamp ||
          desc->root.draw.emulate_viewport_z) {
         /* Ensure viewport depth ranges are up to date now, since we are
          * dirtying root anyway. */
         for (uint32_t i = 0; i < dyn->vp.viewport_count; i++) {
            const VkViewport *vp = &dyn->vp.viewports[i];

            /* These are mutually exclusive. Clamp expects the actual minimum
             * and maximum values, viewport transform supports inverted depth */
            if (desc->root.draw.emulate_depth_clamp) {
               desc->root.draw.viewport_z_range[i * 2] =
                  MIN2(vp->minDepth, vp->maxDepth);
               desc->root.draw.viewport_z_range[i * 2 + 1] =
                  MAX2(vp->minDepth, vp->maxDepth);
            } else {
               desc->root.draw.viewport_z_range[i * 2] = vp->minDepth;
               desc->root.draw.viewport_z_range[i * 2 + 1] = vp->maxDepth;
            }
         }
      }
      desc->root_dirty = true;
   }

   if ((gfx->encoder_dirty || IS_DIRTY(DS_DEPTH_BOUNDS_TEST_ENABLE) ||
        IS_DIRTY(DS_DEPTH_BOUNDS_TEST_BOUNDS)) &&
       kk_depth_bounds_native(pdev)) {
      /* Metal does not expose a separate flag for enabling the depth bounds
       * test. Instead, it treats [0, 1] as disabled. */
      bool bounds_enable = dyn->ds.depth.bounds_test.enable;
      float bounds_min = bounds_enable ? dyn->ds.depth.bounds_test.min : 0.0f;
      float bounds_max = bounds_enable ? dyn->ds.depth.bounds_test.max : 1.0f;
      mtl_set_depth_test_bounds(enc, bounds_min, bounds_max);
   }

   if (gfx->encoder_dirty || IS_DIRTY(DS_STENCIL_REFERENCE))
      mtl_set_stencil_references(
         enc, cmd->vk.dynamic_graphics_state.ds.stencil.front.reference,
         cmd->vk.dynamic_graphics_state.ds.stencil.back.reference);

   if (IS_DIRTY(CB_BLEND_CONSTANTS)) {
      static_assert(sizeof(desc->root.draw.blend_constant) ==
                       sizeof(dyn->cb.blend_constants),
                    "common size");

      memcpy(desc->root.draw.blend_constant, dyn->cb.blend_constants,
             sizeof(dyn->cb.blend_constants));
      desc->root_dirty = true;
   }

   if (IS_DIRTY(VI) || IS_DIRTY(VI_BINDINGS_VALID) ||
       IS_DIRTY(VI_BINDING_STRIDES) || gfx->dirty & KK_DIRTY_VB ||
       (IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(MESH))) {
      if (!kk_flush_vertex_input(cmd))
         return;
      desc->root_dirty = true;
   }

   if (gfx->shader->info.vs.dynamic_multisample &&
       (IS_DIRTY(MS_SAMPLE_MASK) || IS_DIRTY(MS_ALPHA_TO_COVERAGE_ENABLE) ||
        IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(MESH) || IS_SHADER_DIRTY(FRAGMENT))) {
      desc->root.draw.sample_mask = dyn->ms.sample_mask;
      desc->root.draw.alpha_to_coverage = dyn->ms.alpha_to_coverage_enable;
      desc->root_dirty = true;
   }

   struct kk_shader *rate_fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   if ((rate_fs && rate_fs->info.fs.fsr_slots) ||
       (gfx->fsr_coarse && gfx->shader->info.vs.fsr_coarse_slots)) {
      /* Sample invocations of one fragment find each other by draw. The low
       * byte is the render pass epoch: Metal runs the fragments of all draws
       * of a pass tile by tile, so a slot stays claimed for its whole pass. */
      util_call_once_data(&dev->fsr_slots_once, kk_init_fsr_slots, dev);
      struct kk_rendering_state *render = &cmd->state.gfx.render;
      if (!render->fsr_epoch)
         render->fsr_epoch = 0x100u | (p_atomic_inc_return(&dev->fsr_pass) & 0xffu);
      desc->root.fsr.serial = (p_atomic_inc_return(&dev->fsr_serial) << 8) |
                              (render->fsr_epoch & 0xffu);
      desc->root.fsr.slots = dev->fsr_slots ? dev->fsr_slots->gpu : 0;
      desc->root_dirty = true;
   }

   if (desc->root_dirty)
      kk_upload_descriptor_root(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);

   if (desc->root.addr) {
      kk_cmd_bind_root_to_argument_table(cmd, desc->root.addr);
   }

   if (cmd->uses_occlusion_queries &&
       (gfx->encoder_dirty || (gfx->dirty & KK_DIRTY_OCCLUSION))) {
      if (gfx->occlusion.mode != MTL_VISIBILITY_RESULT_MODE_DISABLED)
         cmd->encoder_occlusion = true;
      mtl_set_visibility_result_mode(enc, gfx->occlusion.mode,
                                     gfx->occlusion.index * sizeof(uint64_t));
   }
}

static void
kk_flush_gfx_state(struct kk_cmd_buffer *cmd)
{
   KK_PERF_ENCODER_SCOPE(kk_cmd_buffer_device(cmd), KK_PERF_ENCODER_STATE, state);
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_descriptor_state *desc = &gfx->descriptors;

   kk_flush_render_pass(cmd);
   kk_flush_pipeline(cmd);
   if (gfx->render.view_mask)
      kk_cmd_buffer_apply_view_mask(cmd);

   if (desc->push_dirty)
      kk_cmd_buffer_flush_push_descriptors(cmd, desc);

   /* Only changed draw state on a writable pass needs a new output mask.
    * Physical encoder creation dirties this state through the existing path.
    */
   if (cmd->writable_msaa_render_attachments &&
       (gfx->encoder_dirty || IS_SHADER_DIRTY(FRAGMENT) || IS_DIRTY(COLOR_ATTACHMENT_MAP) ||
        IS_DIRTY(CB_COLOR_WRITE_ENABLES) || IS_DIRTY(CB_WRITE_MASKS) ||
        IS_DIRTY(RS_RASTERIZER_DISCARD_ENABLE)))
      kk_writable_msaa_update_draw(cmd);

   kk_flush_dynamic_state(cmd);

   cmd->state.dirty_shaders = 0u;
   cmd->state.gfx.dirty = 0u;
   cmd->state.gfx.encoder_dirty = false;
   vk_dynamic_graphics_state_clear_dirty(&cmd->vk.dynamic_graphics_state);
}
#undef IS_SHADER_DIRTY
#undef IS_DIRTY

/* Returns true if the draw was successfully converted. */
static bool
kk_convert_to_indirect_draw(struct kk_cmd_buffer *cmd,
                            struct kk_draw_command *data)
{
   if (data->indirect)
      return true;

   uint32_t draw_stride = data->indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                        : sizeof(VkDrawIndirectCommand);
   struct kk_ptr indirect_draw =
      kk_pool_upload(cmd, &data->draws[0], data->draw_count * draw_stride, 4u);

   if (unlikely(!indirect_draw.gpu))
      return false;

   data->indirect_command.addr = indirect_draw.gpu;
   data->indirect_command.stride = draw_stride;
   data->indirect = true;

   return true;
}

/* Returns true if the call succeeds. */
static bool
kk_predicate_draws(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   assert(data->predicate_count);

   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;

   assert((data->indirect_command.stride % sizeof(uint32_t)) == 0 &&
          "stride is not aligned");

   uint32_t out_stride = data->indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                       : sizeof(VkDrawIndirectCommand);
   struct kk_ptr patched =
      kk_pool_alloc(cmd, out_stride * data->draw_count, 4u);
   if (unlikely(!patched.gpu))
      return false;

   uint64_t in_addr = data->indirect_command.addr;
   uint32_t out_stride_el = out_stride / sizeof(uint32_t);
   uint32_t in_stride_el = data->indirect_command.stride / sizeof(uint32_t);

   /* TODO_KOSMICKRISP: This can be accomplished more efficiently using device
    * generated commands, constructing an indirect command buffer on the GPU
    * which only contains the commands to run if the condition is true. For the
    * time being, we apply predicates by zeroing out disabled indirect data */
   struct kk_grid grid = kk_grid_1d(data->draw_count);
   for (uint32_t i = 0; i < data->predicate_count; i++) {
      uint64_t addr = data->predicate_addr[i];
      switch (data->predicate_op[i]) {
      case KK_PREDICATE_GT_DRAW_ID:
         libkk_predicate_indirect_gt_draw_id(cmd, grid, true, patched.gpu,
                                             in_addr, addr, out_stride_el,
                                             in_stride_el);
         break;
      case KK_PREDICATE_EQ_ZERO:
         libkk_predicate_indirect_eq_zero(cmd, grid, true, patched.gpu, in_addr,
                                          addr, out_stride_el, in_stride_el);
         break;
      case KK_PREDICATE_NEQ_ZERO:
         libkk_predicate_indirect_neq_zero(cmd, grid, true, patched.gpu,
                                           in_addr, addr, out_stride_el,
                                           in_stride_el);
         break;
      default:
         UNREACHABLE("Unsupported indirect draw predicate");
      }

      if (i == 0) {
         /* Further predicates will operate on previous patched data */
         in_addr = patched.gpu;
         in_stride_el = out_stride_el;
      }
   }

   data->indirect_command.addr = patched.gpu;
   data->indirect_command.stride = out_stride;
   data->predicate_count = 0;

   return true;
}

/* Points the draw at the 32-bit heap indices an unroll kernel wrote */
static void
kk_set_unrolled_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data,
                     enum mesa_prim prim, uint64_t out_draws)
{
   data->prim = prim;
   data->index_buffer = kk_heap_indices(cmd);
   data->restart_index = UINT32_MAX;
   data->index_buffer_el_size_B = 4u;
   data->indirect = true;
   data->indexed = true;
   data->restart = false;
   data->flatshade_first = true;
   data->indirect_command.addr = out_draws;
   data->indirect_command.stride = sizeof(VkDrawIndexedIndirectCommand);
}

/* Largest direct fan converted on the CPU; larger fans use the unroll kernel. */
#define KK_CPU_FAN_MAX_TRIANGLES 4096u

/* Converts a direct, non-indexed triangle fan into an indexed triangle list
 * whose indices are written on the CPU, in the vertex order the unroll kernel
 * produces. No compute encoder is needed, so the current render pass stays
 * open. Returns false when the draw needs the unroll kernel. */
static bool
kk_fan_to_list(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   if (data->prim != MESA_PRIM_TRIANGLE_FAN || data->indexed ||
       data->indirect || data->draw_count != 1u)
      return false;

   const VkDrawIndirectCommand draw = data->draws[0];
   if (draw.vertexCount < 3u ||
       draw.vertexCount - 2u > KK_CPU_FAN_MAX_TRIANGLES ||
       draw.firstVertex > (uint32_t)INT32_MAX)
      return false;

   const uint32_t triangles = draw.vertexCount - 2u;
   const uint32_t size_B = triangles * 3u * sizeof(uint32_t);
   struct kk_ptr indices = kk_pool_alloc(cmd, size_B, 4u);
   if (unlikely(!indices.gpu || !indices.cpu))
      return false;

   /* Vulkan orders fan triangle i as (i + 1, i + 2, 0) when the first vertex
    * provokes. With the last vertex provoking, (0, i + 1, i + 2) is rotated
    * to (i + 2, 0, i + 1) so that the provoking vertex comes first. */
   uint32_t *out = indices.cpu;
   const bool first = data->flatshade_first;
   for (uint32_t i = 0; i < triangles; i++) {
      out[3u * i + 0u] = first ? i + 1u : i + 2u;
      out[3u * i + 1u] = first ? i + 2u : 0u;
      out[3u * i + 2u] = first ? 0u : i + 1u;
   }

   data->prim = MESA_PRIM_TRIANGLES;
   data->index_buffer = (struct kk_addr_range){
      .addr = indices.gpu,
      .range = size_B,
   };
   data->index_buffer_el_size_B = 4u;
   data->restart_index = UINT32_MAX;
   data->indexed = true;
   data->restart = false;
   data->flatshade_first = true;
   data->indexed_draws[0] = (VkDrawIndexedIndirectCommand){
      .indexCount = triangles * 3u,
      .instanceCount = draw.instanceCount,
      .firstIndex = 0u,
      .vertexOffset = (int32_t)draw.firstVertex,
      .firstInstance = draw.firstInstance,
   };

   if (unlikely(kk_cmd_buffer_device(cmd)->perf != NULL))
      kk_perf_add(kk_cmd_buffer_device(cmd)->perf, KK_PERF_FAN_LIST, 0);
   return true;
}

/* Counts a draw-time kernel that closes the open render encoder. */
static void
kk_count_unroll_split(struct kk_cmd_buffer *cmd)
{
   if (unlikely(kk_cmd_buffer_device(cmd)->perf != NULL) && cmd->metal.render)
      kk_perf_add(kk_cmd_buffer_device(cmd)->perf, KK_PERF_RENDER_SPLIT_UNROLL, 0);
}

/* Unrolling will always be done through indirect rendering, so if this is
 * called from non-indirect calls, we will fake it. */
/* With emulate_flatshade_last, last-vertex provoking triangles are rotated so
 * the provoking vertex comes first. The PerVertexKHR emulation needs the
 * Vulkan vertex order instead and handles the provoking vertex itself. */
static bool
kk_unroll_geometry(struct kk_cmd_buffer *cmd, struct kk_draw_command *data,
                   bool emulate_flatshade_last)
{
   kk_count_unroll_split(cmd);
   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;

   assert((data->indirect_command.stride % sizeof(uint32_t)) == 0 &&
          "stride is not aligned");

   struct kk_ptr out_draws = kk_pool_alloc(
      cmd, data->draw_count * sizeof(VkDrawIndexedIndirectCommand), 4u);
   if (unlikely(!out_draws.gpu))
      return false;

   struct libkk_unroll_geometry_args info = {
      .index_buffer = data->index_buffer.addr,
      .heap = kk_heap(cmd),
      .in_draw = data->indirect_command.addr,
      .out_draw = out_draws.gpu,
      .in_draw_stride_el = data->indirect_command.stride / sizeof(uint32_t),
      /* Handle primitive restart disable by forcing index to UINT32_MAX */
      .restart_index = !data->restart ? UINT32_MAX : data->restart_index,
      .index_buffer_size_el =
         data->indexed ? data->index_buffer.range / data->index_buffer_el_size_B
                       : 0u,
      .in_el_size_B = data->index_buffer_el_size_B,
      .out_el_size_B = 4u,
      .flatshade_first = data->flatshade_first,
      .emulate_flatshade_last = emulate_flatshade_last,
      .mode = data->prim,
   };

   libkk_unroll_geometry_struct(cmd, kk_grid_1d(1024 * data->draw_count), true,
                                info);

   kk_set_unrolled_draw(cmd, data, u_decomposed_prim(data->prim),
                        out_draws.gpu);

   return true;
}

/* Metal has no adjacency topologies, so draws without a GS draw the lines or
 * triangles they rasterize instead. */
static bool
kk_unroll_adjacency(struct kk_cmd_buffer *cmd, struct kk_draw_command *data,
                    bool emulate_flatshade_last)
{
   kk_count_unroll_split(cmd);
   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;

   struct kk_ptr out_draws = kk_pool_alloc(
      cmd, data->draw_count * sizeof(VkDrawIndexedIndirectCommand), 4u);
   if (unlikely(!out_draws.gpu))
      return false;

   struct libkk_unroll_adjacency_args info = {
      .index_buffer = data->index_buffer.addr,
      .heap = kk_heap(cmd),
      .in_draw = data->indirect_command.addr,
      .out_draw = out_draws.gpu,
      .in_draw_stride_el = data->indirect_command.stride / sizeof(uint32_t),
      .index_buffer_size_el =
         data->indexed ? data->index_buffer.range / data->index_buffer_el_size_B
                       : 0u,
      .in_el_size_B = data->index_buffer_el_size_B,
      .flatshade_first = data->flatshade_first,
      .emulate_flatshade_last = emulate_flatshade_last,
      .mode = data->prim,
   };

   libkk_unroll_adjacency_struct(cmd, kk_grid_1d(1024 * data->draw_count), true,
                                 info);

   kk_set_unrolled_draw(cmd, data, u_reduced_prim(data->prim), out_draws.gpu);

   return true;
}

static enum mtl_primitive_type
mesa_prim_to_mtl_primitive_type(enum mesa_prim prim)
{
   switch (prim) {
   case MESA_PRIM_POINTS:
      return MTL_PRIMITIVE_TYPE_POINT;
   case MESA_PRIM_LINES:
      return MTL_PRIMITIVE_TYPE_LINE;
   case MESA_PRIM_LINE_STRIP:
      return MTL_PRIMITIVE_TYPE_LINE_STRIP;
   case MESA_PRIM_TRIANGLES:
      return MTL_PRIMITIVE_TYPE_TRIANGLE;
   case MESA_PRIM_TRIANGLE_STRIP:
      return MTL_PRIMITIVE_TYPE_TRIANGLE_STRIP;
   default:
      UNREACHABLE("Unsupported primitive type");
   }
}

static uint32_t
build_per_draw_upload_mask(struct kk_cmd_buffer *cmd)
{
   uint32_t mask = 0;

   struct kk_shader *vertex = cmd->state.gfx.shader;
   if (vertex && vertex->info.uses_per_draw_data) {
      mask |= BITFIELD_BIT(MESA_SHADER_VERTEX);
   }

   /* Tessellation will always require per draw data to be submitted. */
   struct kk_shader *tese = cmd->state.shaders[MESA_SHADER_TESS_EVAL];
   if (tese) {
      mask |= BITFIELD_BIT(MESA_SHADER_TESS_EVAL);
   }

   /* So does a GS */
   if (kk_geometry_shader(cmd)) {
      mask |= BITFIELD_BIT(MESA_SHADER_GEOMETRY);
   }

   struct kk_shader *fragment = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   if (fragment && fragment->info.uses_per_draw_data) {
      mask |= BITFIELD_BIT(MESA_SHADER_FRAGMENT);
   }

   return mask;
}

static struct kk_addr_range
kk_sanitize_index_range(struct kk_cmd_buffer *cmd, struct kk_addr_range range)
{
   /* Metal does not support an index buffer address or size of 0. Cache a pool
    * allocation of the maximum index size, filled with 0, and use it to handle
    * these cases. Robustness will handle over-reads.
    *
    * Note that we only need to do this at draw dispatch as our other draw logic
    * for unrolling, tessellation, etc. will properly handle these cases. */
   if (range.addr == 0u || range.range == 0u) {
      struct kk_graphics_state *gfx = &cmd->state.gfx;

      if (!gfx->index.null_addr) {
         struct kk_ptr null_alloc =
            kk_pool_alloc(cmd, sizeof(uint32_t), sizeof(uint32_t));
         *(uint32_t *)null_alloc.cpu = 0u;

         gfx->index.null_addr = null_alloc.gpu;
      }

      range.addr = gfx->index.null_addr;
      range.range = sizeof(uint32_t);
   }

   return range;
}

static void
kk_dispatch_draw(struct kk_cmd_buffer *cmd, struct kk_draw_data data)
{
   mtl_render_encoder *enc = cs_get_render(cmd);
   /* Actual draw execution cannot be inferred from pipeline binding: a later
    * storage-only draw must not publish an earlier, unexecuted color state. */
   cmd->writable_msaa_render_writes |= cmd->writable_msaa_draw_writes;

   if (kk_grid_is_indirect(data.grid)) {
      if (data.index.el_size_B) {
         struct kk_addr_range index_range =
            kk_sanitize_index_range(cmd, data.index.gpu);
         mtl_draw_indexed_primitives_indirect(
            enc, data.primitive_type,
            index_size_in_bytes_to_mtl_index_type(data.index.el_size_B),
            index_range.addr, index_range.range, data.grid.addr);
      } else {
         mtl_draw_primitives_indirect(enc, data.primitive_type, data.grid.addr);
      }
   } else {
      if (data.index.el_size_B) {
         struct kk_addr_range index_range =
            kk_sanitize_index_range(cmd, data.index.gpu);
         mtl_draw_indexed_primitives(
            enc, data.primitive_type, data.grid.size.x,
            index_size_in_bytes_to_mtl_index_type(data.index.el_size_B),
            index_range.addr, index_range.range, data.grid.size.y,
            data.vertex_offset, data.grid.size.z);
      } else {
         /* Avoid Metal validation error. Empty draws from tessellation will
          * have values set to 0. */
         if (data.grid.size.x != 0 && data.grid.size.y != 0)
            mtl_draw_primitives(enc, data.primitive_type, data.vertex_offset,
                                data.grid.size.x, data.grid.size.y,
                                data.grid.size.z);
      }
   }
}

static bool
requires_unroll_index_promotion(const struct kk_draw_command *data)
{
   if (!data->indexed)
      return false;

   /* uint8_t indices must be promoted since they are not natively supported. */
   if (data->index_buffer_el_size_B == sizeof(uint8_t))
      return true;

   /* For primitive types that support primitive restart, if restart is disabled
    * and the index size is less than uint32_t, we need to make sure to perform
    * unrolling as it automatically promotes the type to uint32_t, preventing
    * valid indices from being treated as restarts. For uint32_t indices with
    * restart disabled, we realistically will never have enough vertices for the
    * restart index to be valid anyway. */
   switch (data->prim) {
   case MESA_PRIM_LINE_STRIP:
   case MESA_PRIM_TRIANGLE_STRIP:
   case MESA_PRIM_TRIANGLE_FAN:
      return (!data->restart &&
              data->index_buffer_el_size_B < sizeof(uint32_t));
   default:
      return false;
   }
}

static bool
requires_unroll_robustness(struct kk_cmd_buffer *cmd,
                           const struct kk_draw_command *data)
{
   /* No need for robustness if the draw does not use an index buffer */
   if (!data->indexed)
      return false;

   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   /* KK_WORKAROUND_12, KK_WORKAROUND_13 */
   bool workaround_12 =
      !(pdev->settings.disabled_workarounds & BITFIELD64_BIT(12));
   bool workaround_13 =
      !(pdev->settings.disabled_workarounds & BITFIELD64_BIT(13));

   /* Do nothing if both workarounds are disabled */
   if (!workaround_12 && !workaround_13)
      return false;

   /* KK_WORKAROUND_12: Need to handle null descriptor case here as we can't
    * rely on hardware robustness */
   if (workaround_12 &&
       (data->index_buffer.addr == 0u || data->index_buffer.range == 0u))
      return true;

   /* No need to handle robustness if not enabled
    * TODO_KOSMICKRISP: Narrow pipeline robustness to vertex input only */
   if (!dev->vk.enabled_features.robustBufferAccess2 &&
       !dev->vk.enabled_features.pipelineRobustness)
      return false;

   /* Only need to handle case where index buffer is not aligned to 4 bytes.
    * KK_WORKAROUND_12: Need to handle all alignments. */
   if (!workaround_12 && (data->index_buffer.addr & 3u) == 0u &&
       (data->index_buffer.range & 3u) == 0u)
      return false;

   /* We can't tell if the draw over-reads up-front with indirect draws, so we
    * always have to handle it */
   if (data->indirect)
      return true;

   /* For direct draws, we can check now if any over-read the index buffer */
   for (uint32_t i = 0; i < data->draw_count; i++) {
      const VkDrawIndexedIndirectCommand *draw = &data->indexed_draws[i];
      if ((draw->firstIndex + draw->indexCount) * data->index_buffer_el_size_B >
          data->index_buffer.range)
         return true;
   }

   return false;
}

static bool
requires_unroll_restart(struct kk_cmd_buffer *cmd,
                        const struct kk_draw_command *data)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);

   if (!data->restart || !data->indexed)
      return false;

   /* The adjacency rewrite reads restart-free input */
   if (mesa_prim_has_adjacency(data->prim))
      return true;

   switch (data->prim) {
   case MESA_PRIM_POINTS:
   case MESA_PRIM_LINES:
   case MESA_PRIM_TRIANGLES:
      /* Unroll list restart only if the user requests it, to avoid associated
       * cost otherwise. Some applications unintentionally leave the primitive
       * restart flag enabled while using list primitives without any restarts,
       * and in these cases we can avoid the cost of unroll, even though they
       * are technically against spec. */
      return dev->vk.enabled_features.primitiveTopologyListRestart;
   default:
      break;
   }

   /* For topologies that natively support restart, unroll if unusual primitive
    * restart index is set by user */
   uint32_t default_idx = BITFIELD_RANGE(0, data->index_buffer_el_size_B * 8);
   return data->restart_index != default_idx;
}

static bool
requires_unroll_flatshade(struct kk_cmd_buffer *cmd,
                          struct kk_draw_command *data)
{
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];

   /* If the last vertex is provoking and the fragment shader uses flat inputs,
    * we need to unroll to correct the vertex order */
   return !data->flatshade_first && fs && fs->info.fs.uses_flat_varyings;
}

static bool
requires_unroll(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   /* No need to unroll for tessellation. Robustness is handled by tessellator,
    * restart is not supported for patch lists, and flat-shading does not have a
    * well-defined vertex used when tessellating. A GS reads any index size and
    * topology through poly's bounds-checked input assembly, and
    * kk_draw_without_restart unrolls restart before it runs. */
   bool tess = cmd->state.shaders[MESA_SHADER_TESS_EVAL];
   bool geom = kk_geometry_shader(cmd);
   if (tess || geom)
      return false;

   /* Metal does not support triangle fans */
   if (data->prim == MESA_PRIM_TRIANGLE_FAN)
      return true;

   /* Check for remaining unroll conditions */
   return requires_unroll_index_promotion(data) ||
          requires_unroll_robustness(cmd, data) ||
          requires_unroll_restart(cmd, data) ||
          requires_unroll_flatshade(cmd, data);
}

static uint64_t
upload_base_vertex(struct kk_cmd_buffer *cmd, const struct kk_draw_data *data)
{
   if (kk_grid_is_indirect(data->grid)) {
      uint64_t first_vertex_offset =
         data->index.el_size_B
            ? offsetof(VkDrawIndexedIndirectCommand, vertexOffset)
            : offsetof(VkDrawIndirectCommand, firstVertex);
      return data->grid.addr + first_vertex_offset;
   } else {
      return kk_pool_upload(cmd, &data->vertex_offset,
                            sizeof(data->vertex_offset), 4u)
         .gpu;
   }
}

static uint64_t
upload_base_instance(struct kk_cmd_buffer *cmd, const struct kk_draw_data *data)
{
   if (kk_grid_is_indirect(data->grid)) {
      uint64_t base_instance_offset =
         data->index.el_size_B
            ? offsetof(VkDrawIndexedIndirectCommand, firstInstance)
            : offsetof(VkDrawIndirectCommand, firstInstance);
      return data->grid.addr + base_instance_offset;
   } else {
      return kk_pool_upload(cmd, &data->grid.size.z, sizeof(data->grid.size.z),
                            4u)
         .gpu;
   }
}

/* Prepares emulation data for stages like tessellation. It also upload system
 * values not present in Metal such as drawID. gs_restart is set when a GS draw
 * unrolls primitive restart. */
#include "kk_poly_retained_raster_command.h"
#include "kk_poly_layer_replay_command.h"

static void
kk_upload_per_draw_data(struct kk_cmd_buffer *cmd, uint32_t upload_mask,
                        uint32_t draw_id, const struct kk_draw_data *draw,
                        bool gs_restart)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   gfx->per_draw_data.draw_id = draw_id;

   /* Prepare emulation data for tessellation and GS. */
   bool tess = upload_mask & BITFIELD_BIT(MESA_SHADER_TESS_EVAL);
   bool geom = upload_mask & BITFIELD_BIT(MESA_SHADER_GEOMETRY);
   if (tess || geom) {
      gfx->per_draw_data.index_size = draw->index.el_size_B;
      gfx->per_draw_data.base_vertex_addr = upload_base_vertex(cmd, draw);
      gfx->per_draw_data.base_instance_addr = upload_base_instance(cmd, draw);
      gfx->per_draw_data.vertex_params = kk_upload_vertex_params(cmd, draw);
   }

   if (tess) {
      struct kk_ptr tess_args =
         kk_pool_alloc(cmd, sizeof(struct poly_tess_params), 4);
      gfx->per_draw_data.tess_params = tess_args.gpu;
      if (tess_args.gpu) {
         kk_upload_tess_params(cmd, tess_args.cpu, draw);
      }
      struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
      if (!geom && fs && fs->info.fs.emulates_per_vertex_inputs) {
         struct kk_device *dev = kk_cmd_buffer_device(cmd);
         struct kk_per_draw_data *pd = &gfx->per_draw_data;
         pd->per_vertex_indirect_addr = gfx->tess.out_draws_addr;
         pd->per_vertex_index_addr = kk_draw_heap(cmd)->gpu + sizeof(struct poly_heap);
         pd->per_vertex_index_size = 4u;
         pd->per_vertex_index_count =
            (kk_draw_heap(cmd)->size_B - sizeof(struct poly_heap)) / 4u;
         pd->per_vertex_first_vertex = 0u;
         pd->per_vertex_base_vertex = 0;
         pd->per_vertex_topology = gfx->tess.prim;
         pd->per_vertex_heap =
            fs->info.fs.per_vertex_uses_heap ? kk_heap(cmd) : 0u;
      }

   }

   if (geom) {
      gfx->per_draw_data.geometry_params =
         kk_upload_geometry_params(cmd, draw, gs_restart);

      const struct vk_dynamic_graphics_state *dyn =
         &cmd->vk.dynamic_graphics_state;
      gfx->per_draw_data.provoking_last =
         dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   }

   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   bool per_vertex = fs && fs->info.fs.emulates_per_vertex_inputs;
   if (per_vertex)
      gfx->per_draw_data.provoking_last =
         cmd->vk.dynamic_graphics_state.rs.provoking_vertex ==
         VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   /* The optional tail adds no upload bytes to ordinary shaders. */
   bool multiview = gfx->render.view_mask &&
      (cmd->state.shaders[MESA_SHADER_TESS_EVAL] ||
       cmd->state.gfx.shader->info.api_geometry ||
       gfx->layer_replay.bind || gfx->polygon_points);
   uint32_t per_draw_size = geom && gfx->shader->info.geometry_statistics &&
      (cmd->state.pipeline_statistics_flags & KK_GEOMETRY_QUERY_BITS) ?
      sizeof(gfx->per_draw_data) :
      multiview ? offsetof(struct kk_per_draw_data, geometry_statistics) :
      per_vertex ? offsetof(struct kk_per_draw_data, multiview_index) :
      offsetof(struct kk_per_draw_data, per_vertex_index_addr);
   struct kk_ptr shader_data_gpu =
      kk_pool_upload(cmd, &gfx->per_draw_data, per_draw_size, 8u);
   if (unlikely(!shader_data_gpu.gpu))
      return;

   mtl_set_address(cmd->argument_table, shader_data_gpu.gpu, KK_ARGUMENT_PER_DRAW);
   if (gfx->layer_replay.bind) gfx->layer_replay.per_draw_address = shader_data_gpu.gpu;
   if (gfx->poly_retained.consume) gfx->poly_retained.per_draw_address = shader_data_gpu.gpu;
}

static void
kk_dispatch_compute(mtl_compute_encoder *enc, struct kk_grid grid,
                    struct mtl_size local_size)
{
   if (grid.mode == KK_GRID_DIRECT)
      mtl_dispatch_threads(enc, grid.size, local_size);
   else if (grid.mode == KK_GRID_INDIRECT_LOCAL)
      mtl_dispatch_threads_with_indirect_buffer(enc, grid.addr);
   else
      mtl_dispatch_threadgroups_with_indirect_buffer(enc, grid.addr,
                                                     local_size);
}

static struct kk_draw_data
kk_launch_tess(struct kk_cmd_buffer *cmd, struct kk_draw_data draw)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_grid grid_vs, grid_tcs, grid_tess;

   struct kk_shader *vs = cmd->state.gfx.shader;
   struct kk_shader *tcs = cmd->state.shaders[MESA_SHADER_TESS_CTRL];

   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   uint32_t input_patch_size = dyn->ts.patch_control_points;
   uint64_t state = gfx->per_draw_data.tess_params;
   struct kk_tess_info info = gfx->tess.info;

   /* Setup grids */
   if (kk_grid_is_indirect(draw.grid)) {
      struct libkk_tess_setup_indirect_args args = {
         .p = state,
         .grids = gfx->tess.indirect_ptr.gpu,
         .indirect = draw.grid.addr,
         .vp = gfx->per_draw_data.vertex_params,
         .vertex_outputs = vs->info.outputs_written,
         .tcs_statistic = 0,
         .vs_group_size = KK_POLY_WG_SIZE,
      };

      if (draw.index.el_size_B) {
         args.in_index_buffer = draw.index.gpu.addr;
         args.in_index_size_B = draw.index.el_size_B;
         args.in_index_buffer_range_el =
            draw.index.gpu.range / args.in_index_size_B;
      }

      libkk_tess_setup_indirect_struct(cmd, kk_grid_1d(1), true, args);

      uint32_t grid_stride = sizeof(struct kk_tess_grid);
      grid_vs = kk_grid_indirect_local(gfx->tess.indirect_ptr.gpu + 0u * grid_stride);
      grid_tcs =
         kk_grid_indirect_local(gfx->tess.indirect_ptr.gpu + 1u * grid_stride);
      grid_tess =
         kk_grid_indirect_local(gfx->tess.indirect_ptr.gpu + 2u * grid_stride);
   } else {
      uint32_t patches = draw.grid.size.x / input_patch_size;
      grid_vs = grid_tcs = kk_grid_2d(draw.grid.size.x, draw.grid.size.y);

      grid_tcs.size.x = patches * tcs->info.tess.tcs_output_patch_size;
      grid_tess = kk_grid_1d(patches * draw.grid.size.y);
   }

   /* First launch the VS and TCS */

   mtl_compute_encoder *enc = cs_get_compute(cmd);
   {
      mtl_compute_pipeline_state *pipeline = kk_cmd_sampler_compute_pipeline(
         cmd, vs, vs->pipeline.gfx.pre_render[0]);
      if (!pipeline) return (struct kk_draw_data){.grid = kk_grid_1d(0)};
      struct mtl_size local_size = {KK_POLY_WG_SIZE, 1, 1};
      KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, enc, MTL_STAGE_DISPATCH,
                                       MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
      mtl_compute_set_pipeline_state(enc, pipeline);
      kk_dispatch_compute(enc, grid_vs, local_size);
   }
   {
      mtl_compute_pipeline_state *pipeline = kk_cmd_sampler_compute_pipeline(
         cmd, vs, vs->pipeline.gfx.pre_render[1]);
      if (!pipeline) return (struct kk_draw_data){.grid = kk_grid_1d(0)};
      struct mtl_size local_size = {tcs->info.tess.tcs_output_patch_size, 1, 1};
      /* Avoid Metal validation error by trying to launch empty compute. Return
       * empty data. We set restart to true to avoid unroll. */
      if (grid_tcs.mode == KK_GRID_DIRECT && grid_tcs.size.x == 0u)
         return (struct kk_draw_data){.grid = kk_grid_1d(0u)};
      KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, enc, MTL_STAGE_DISPATCH,
                                       MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
      mtl_compute_set_pipeline_state(enc, pipeline);
      kk_dispatch_compute(enc, grid_tcs, local_size);
   }

   /* First generate counts, then prefix sum them, and then tessellate. */
   libkk_tessellate(cmd, grid_tess, true, info.mode, POLY_TESS_MODE_COUNT,
                    state);

   libkk_prefix_sum_tess(cmd, kk_grid_1d(1024u), true, state);

   libkk_tessellate(cmd, grid_tess, true, info.mode, POLY_TESS_MODE_WITH_COUNTS,
                    state);

   if (cmd->state.pipeline_statistics_flags) {
      bool clip = !kk_geometry_shader(cmd);
      uint64_t tcs_stat = kk_pipeline_stat_addr(cmd,
         VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT);
      uint64_t tes_stat = kk_pipeline_stat_addr(cmd,
         VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT);
      uint64_t ci = clip ? kk_pipeline_stat_addr(cmd,
         VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT) : 0;
      uint64_t cp = clip ? kk_pipeline_stat_addr(cmd,
         VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT) : 0;
      if (tcs_stat || tes_stat || ci || cp)
         libkk_accumulate_tess_statistics(cmd, kk_grid_1d(1), true, state,
            tcs_stat, tes_stat, ci, cp, cmd->interlock_heap_active ? 1 :
            MAX2(1, util_bitcount(gfx->render.view_mask)));
   }

   draw.grid = kk_grid_indirect(gfx->tess.out_draws_addr);

   draw.index.gpu = kk_heap_indices(cmd);
   draw.index.el_size_B = 4u;
   draw.primitive_type = mesa_prim_to_mtl_primitive_type(gfx->tess.prim);
   return draw;
}

/* A GS assembles its input in software, which cannot follow restarts, so the
 * draw is unrolled to lists first. */
static struct kk_draw_data
kk_draw_without_restart(struct kk_cmd_buffer *cmd,
                        const struct kk_draw_command *data,
                        struct kk_draw_data draw)
{
   assert(kk_grid_is_indirect(draw.grid));

   struct kk_ptr out_draw =
      kk_pool_alloc(cmd, sizeof(VkDrawIndexedIndirectCommand), 4u);

   struct libkk_unroll_restart_args args = {
      .index_buffer = draw.index.gpu.addr,
      .heap = kk_heap(cmd),
      .in_draw = draw.grid.addr,
      .out_draw = out_draw.gpu,
      .restart_index = data->restart_index,
      .capture = cmd->state.gfx.polygon_points &&
         cmd->vk.dynamic_graphics_state.rs.line.stipple.enable ? cmd->state.gfx.polygon_capture : 0,
      .index_buffer_size_el = draw.index.gpu.range / draw.index.el_size_B,
      .index_size_B = draw.index.el_size_B,
      .flatshade_first = data->flatshade_first,
      .mode = data->prim,
   };
   libkk_unroll_restart_struct(cmd, kk_grid_1d(1024u), true, args);

   draw.grid = kk_grid_indirect(out_draw.gpu);
   draw.index.gpu = kk_heap_indices(cmd);
   return draw;
}

#include "kk_interlock_source_command.h"
static mtl_compute_pipeline_state *
kk_pre_gs_pipeline(struct kk_cmd_buffer *cmd, struct kk_shader *shader,
                   bool statistics)
{
   mtl_compute_pipeline_state *pipeline = shader->pipeline.gfx.gs_aux[1];
   if (statistics) {
      VkResult result = kk_shader_get_statistics_pre_gs(
         kk_cmd_buffer_device(cmd), shader, &pipeline);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return NULL;
      }
   }
   return kk_cmd_sampler_compute_pipeline(cmd, shader, pipeline);
}

#include "kk_polygon_command.h"

/* Runs the stage feeding the GS and the GS compute pass, returning the draw
 * that rasterizes its output. */
static struct kk_draw_data
kk_launch_gs_prerast(struct kk_cmd_buffer *cmd,
                     const struct kk_draw_command *data,
                     struct kk_draw_data draw, bool restart)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *vs = cmd->state.gfx.shader;
   struct kk_shader *tes = cmd->state.shaders[MESA_SHADER_TESS_EVAL];
   struct kk_shader *gs = kk_geometry_shader(cmd);
   uint64_t vertex_params = gfx->per_draw_data.vertex_params;
   uint64_t geometry_params = gfx->per_draw_data.geometry_params;
   enum poly_gs_shape shape = gs->info.geometry.shape;
   struct kk_grid grid_vs, grid_gs;

   enum mesa_prim mode = kk_gs_in_prim(cmd);

   if (restart) {
      draw = kk_draw_without_restart(cmd, data, draw);
      mode = u_decomposed_prim(mode);
   }

   if (kk_grid_is_indirect(draw.grid)) {
      struct libkk_gs_setup_indirect_args args = {
         .index_buffer = draw.index.gpu.addr,
         .draw = draw.grid.addr,
         .vp = vertex_params,
         .p = geometry_params,
         .heap = kk_heap(cmd),
         .vs_outputs = (tes ? tes : vs)->info.outputs_written,
         .prim = mode,
         .is_prefix_summing = gs->info.geometry.prefix_sum,
         .max_indices = gs->info.geometry.max_indices,
         .shape = shape,
      };

      if (draw.index.el_size_B) {
         args.index_size_B = draw.index.el_size_B;
         args.index_buffer_range_el =
            draw.index.gpu.range / draw.index.el_size_B;
      }

      libkk_gs_setup_indirect_struct(cmd, kk_grid_1d(1u), true, args);

      grid_vs = kk_grid_indirect_local(
         vertex_params + offsetof(struct poly_vertex_params, grid));
      grid_gs = kk_grid_indirect_local(
         geometry_params + offsetof(struct poly_geometry_params, grid));
   } else {
      grid_vs = grid_gs = kk_grid_2d(draw.grid.size.x, draw.grid.size.y);
      grid_gs.size.x = u_decomposed_prims_for_vertices(mode, draw.grid.size.x);

      /* Avoid Metal validation errors from empty dispatches and draws */
      if (grid_gs.size.x == 0u)
         return (struct kk_draw_data){.grid = kk_grid_1d(0u)};
   }

   /* The stage feeding the GS and its compute pass are the last two pre-render
    * pipelines */
   uint32_t pre_render_count = vs->pipeline.gfx.pre_render_count;
   struct mtl_size local_size = {KK_POLY_WG_SIZE, 1, 1};
   mtl_compute_encoder *enc = cs_get_compute(cmd);

   KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, enc, MTL_STAGE_DISPATCH,
                                    MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
   mtl_compute_pipeline_state *producer_pipeline = kk_cmd_sampler_compute_pipeline(
      cmd, vs, vs->pipeline.gfx.pre_render[pre_render_count - 2u]);
   if (!producer_pipeline) return (struct kk_draw_data){.grid = kk_grid_1d(0)};
   mtl_compute_set_pipeline_state(enc, producer_pipeline);
   kk_dispatch_compute(enc, grid_vs, local_size);
   if (gfx->polygon_points || cmd->interlock_heap_active)
      return kk_launch_polygon_points(cmd, grid_gs, geometry_params);

   bool statistics = vs->info.geometry_statistics &&
      (cmd->state.pipeline_statistics_flags & KK_GEOMETRY_QUERY_BITS);
   bool queries = statistics || gfx->geometry_query_mask != 0;
   if (gs->info.geometry.xfb || queries) {
      VkResult result = kk_shader_ensure_geometry_reports(kk_cmd_buffer_device(cmd), vs, 3);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return (struct kk_draw_data){.grid = kk_grid_1d(0)};
      }
      if (vs->info.gs_aux_mask & BITFIELD_BIT(0)) {
         KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
         mtl_compute_pipeline_state *count_pipeline = kk_cmd_sampler_compute_pipeline(
            cmd, vs, vs->pipeline.gfx.gs_aux[0]);
         if (!count_pipeline) return (struct kk_draw_data){.grid = kk_grid_1d(0)};
         mtl_compute_set_pipeline_state(enc, count_pipeline);
         kk_dispatch_compute(enc, grid_gs, local_size);
      }
      if (gs->info.geometry.prefix_sum)
         libkk_prefix_sum_geometry(cmd, kk_grid_1d(1024 * gs->info.geometry.count_words), true, geometry_params);
      enc = cs_get_compute(cmd);
      if (vs->info.gs_aux_mask & BITFIELD_BIT(1)) {
         mtl_compute_pipeline_state *pipeline = kk_pre_gs_pipeline(cmd, vs, statistics);
         if (!pipeline)
            return (struct kk_draw_data){.grid = kk_grid_1d(0u)};
         KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
         mtl_compute_set_pipeline_state(enc, pipeline);
         kk_dispatch_compute(enc, kk_grid_1d(1), (struct mtl_size){1, 1, 1});
      }
   }
   KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, enc, MTL_STAGE_DISPATCH,
                                    MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
   mtl_compute_pipeline_state *geometry_pipeline = kk_cmd_sampler_compute_pipeline(
      cmd, vs, vs->pipeline.gfx.pre_render[pre_render_count - 1u]);
   if (!geometry_pipeline) return (struct kk_draw_data){.grid = kk_grid_1d(0)};
   mtl_compute_set_pipeline_state(enc, geometry_pipeline);
   kk_dispatch_compute(enc, grid_gs, local_size);

   struct kk_draw_data rast = {
      .primitive_type = mesa_prim_to_mtl_primitive_type(gs->info.geometry.mode),
   };

   if (poly_gs_indexed(shape)) {
      rast.index.gpu = gfx->gs.index;
      rast.index.el_size_B = shape == POLY_GS_SHAPE_DYNAMIC_INDEXED
                                ? sizeof(uint32_t)
                                : sizeof(uint16_t);
   }

   if (kk_grid_is_indirect(draw.grid)) {
      rast.grid = kk_grid_indirect(geometry_params +
                                   offsetof(struct poly_geometry_params, draw));
   } else {
      rast.grid = kk_grid_3d(gfx->gs.index_count, gfx->gs.instance_count, 0u);
   }

   return rast;
}

/* Get modifiable per draw data. */
static struct kk_draw_data
build_draw_data(struct kk_cmd_buffer *cmd, struct kk_draw_command *data,
                uint32_t draw_id)
{
   /* Emulated stages rasterize a primitive of their own */
   bool emulated = cmd->state.shaders[MESA_SHADER_TESS_EVAL] ||
                   kk_geometry_shader(cmd);
   struct kk_draw_data draw = {
      .index.gpu = data->index_buffer,
      .index.el_size_B = data->index_buffer_el_size_B,
      /* Fans only get here with the PerVertexKHR emulation, which draws them
       * as triangle lists. */
      .primitive_type = emulated ? 0u
                                 : mesa_prim_to_mtl_primitive_type(
                                      data->prim == MESA_PRIM_TRIANGLE_FAN
                                         ? MESA_PRIM_TRIANGLES
                                         : data->prim),
   };

   if (data->indirect) {
      uint64_t addr =
         data->indirect_command.addr + (uint64_t)draw_id * data->indirect_command.stride;
      draw.grid = kk_grid_indirect(addr);
   } else if (data->indexed) {
      VkDrawIndexedIndirectCommand draw_cmd = data->indexed_draws[draw_id];
      draw.grid = kk_grid_3d(draw_cmd.indexCount, draw_cmd.instanceCount,
                             draw_cmd.firstInstance);
      draw.vertex_offset = draw_cmd.vertexOffset;

      uint64_t index_offset =
         draw_cmd.firstIndex * data->index_buffer_el_size_B;
      draw.index.gpu.addr += index_offset;
      draw.index.gpu.range = index_offset < draw.index.gpu.range
                                ? draw.index.gpu.range - index_offset
                                : 0u;
   } else {
      VkDrawIndirectCommand draw_cmd = data->draws[draw_id];
      draw.grid = kk_grid_3d(draw_cmd.vertexCount, draw_cmd.instanceCount,
                             draw_cmd.firstInstance);
      draw.vertex_offset = draw_cmd.firstVertex;
   }

   return draw;
}

#include "kk_statistics_command.h"
#include "kk_mesh_command.h"
#include "kk_dgc_mesh_draw.h"

#include "kk_barycentric_draw.h"
#include "kk_gs_mesh_command.h"

static void
kk_draw_one_view(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   if (!kk_layered_bc_prepare_draw(cmd)) return;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *vs = cmd->state.gfx.shader;
   if (BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_CONSERVATIVE_MODE))
      kk_cmd_buffer_update_conservative(cmd, vs);
   bool point = kk_polygon_expansion_active(cmd);
   if (vs->lazy_polygon && (point || gfx->layer_replay.bind || gfx->poly_retained.consume)) {
      VkResult result = kk_shader_ensure_polygon(kk_cmd_buffer_device(cmd), vs);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         goto layered_bc_done;
      }
   }
   if (point && BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE))
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
   if (point != gfx->polygon_points) {
      gfx->polygon_points = point;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE);
   }

   if (vs->info.interlock_envelope || cmd->interlock_heap_active) {
      VkResult result = kk_interlock_heap_select(cmd,
         vs->info.interlock_envelope &&
         (vs->info.depth_bounds_emulation || dyn->ms.rasterization_samples > 1));
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         goto layered_bc_done;
      }
   }
   kk_flush_gfx_state(cmd);
   if (!kk_layered_bc_begin_draw(cmd, data)) goto layered_bc_done;

   data->restart = dyn->ia.primitive_restart_enable;
   data->restart_index = dyn->ia.primitive_restart_index;
   data->flatshade_first =
      dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;

   /* Convert to indirect and process predicates. Skip draw if we fail. */
   if (data->predicate_count > 0 && !kk_predicate_draws(cmd, data))
      goto layered_bc_done;

   bool tess = cmd->state.shaders[MESA_SHADER_TESS_EVAL];
   bool geom = kk_geometry_shader(cmd);

   kk_update_ia_statistics(cmd, data);

   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   bool emulates_per_vertex = fs && fs->info.fs.emulates_per_vertex_inputs &&
      !gfx->conservative;
   bool per_vertex = !tess && !geom && emulates_per_vertex;

   /* Unroll geometry. Skip draw if we fail. The per-vertex emulation issues
    * its own lists. Direct non-indexed fans are converted on the CPU. */
   if (!per_vertex && requires_unroll(cmd, data) &&
       !kk_fan_to_list(cmd, data) &&
       !kk_unroll_geometry(cmd, data, true))
      goto layered_bc_done;

   /* Primitive restart: split into a triangle list on the GPU, keeping the
    * Vulkan vertex order, then emulate on the list. */
   if (per_vertex && data->indexed && data->restart &&
       !kk_unroll_geometry(cmd, data, false))
      goto layered_bc_done;

   /* Rotate the rewritten primitives only when flat inputs need the last
    * vertex first, like kk_unroll_geometry. The per-vertex emulation handles
    * last-vertex provoking itself and keeps the Vulkan corner order. */
   if (!geom && mesa_prim_has_adjacency(data->prim) &&
       !kk_unroll_adjacency(
          cmd, data, !per_vertex && requires_unroll_flatshade(cmd, data)))
      goto layered_bc_done;

   /* Patch lists do not restart, so only a GS fed by the input assembly needs
    * its restarts unrolled. The unroll kernel reads an indirect draw. */
   bool gs_restart = geom && !tess && data->restart && data->indexed;
   if (gs_restart && !kk_convert_to_indirect_draw(cmd, data))
      goto layered_bc_done;

   uint64_t per_vertex_draws = 0u;
   if (per_vertex && data->indirect && !per_vertex_draws_raw_indirect(data)) {
      struct kk_ptr out = kk_pool_alloc(
         cmd, data->draw_count * sizeof(VkDrawIndirectCommand), 4u);
      if (unlikely(!out.gpu))
         goto layered_bc_done;
      libkk_per_vertex_draw(cmd, kk_grid_1d(data->draw_count), true, out.gpu,
                            data->indirect_command.addr,
                            data->indirect_command.stride / sizeof(uint32_t),
                            data->indexed, data->prim);
      per_vertex_draws = out.gpu;
   }
   bool per_vertex_tess = tess && !geom && emulates_per_vertex;
   bool per_vertex_gs = geom && emulates_per_vertex;

   for (uint32_t i = 0; i < data->draw_count; i++) {
      uint32_t source_views = cmd->interlock_heap_active && gfx->render.view_mask ?
         gfx->render.view_mask : 1;
      unsigned source_ordinal = 0;
      while (source_views) {
         gfx->interlock_source_view = gfx->render.view_mask && cmd->interlock_heap_active ?
            ffs(source_views) - 1 : 0;
         gfx->interlock_source_view_ordinal = source_ordinal++;
         source_views &= source_views - 1;
      struct kk_draw_data draw_data = build_draw_data(cmd, data, i);

      if (per_vertex)
         kk_prepare_per_vertex_draw(cmd, data, i, per_vertex_draws, &draw_data);

      if (geom) {
         if (vs->gs_mesh &&
             kk_draw_gs_mesh(cmd, vs, data, &draw_data, i + gfx->indirect_draw_id))
            continue;
         if (data->upload_mask || cmd->interlock_heap_active)
            kk_upload_per_draw_data(cmd, data->upload_mask |
               (cmd->interlock_heap_active ? BITFIELD_BIT(MESA_SHADER_GEOMETRY) : 0),
               i + gfx->indirect_draw_id, &draw_data, gs_restart);
         if (tess)
            draw_data = kk_launch_tess(cmd, draw_data);
         draw_data = kk_launch_gs_prerast(cmd, data, draw_data, gs_restart);
         kk_finish_geometry_statistics(cmd);
      } else {
         if (data->upload_mask || cmd->interlock_heap_active)
            kk_upload_per_draw_data(cmd, data->upload_mask |
               (cmd->interlock_heap_active ? BITFIELD_BIT(MESA_SHADER_GEOMETRY) : 0),
               i + gfx->indirect_draw_id, &draw_data, gs_restart);
         if (tess)
            draw_data = kk_launch_tess(cmd, draw_data);
      }
      if (per_vertex_gs &&
          !kk_prepare_gs_per_vertex_draw(cmd, data, &draw_data))
         continue;

      if (per_vertex_tess && kk_grid_is_indirect(draw_data.grid)) {
         struct kk_ptr out =
            kk_pool_alloc(cmd, sizeof(VkDrawIndirectCommand), 4u);
         if (unlikely(!out.gpu))
            continue;
         libkk_per_vertex_draw(
            cmd, kk_grid_1d(1u), true, out.gpu, draw_data.grid.addr,
            sizeof(VkDrawIndexedIndirectCommand) / sizeof(uint32_t), 1u,
            cmd->state.gfx.tess.prim);
         draw_data.grid = kk_grid_indirect(out.gpu);
         draw_data.index.el_size_B = 0u;
      }


      if (!geom && gfx->primitives_query[0]) {
         enum mesa_prim mode = tess ? gfx->tess.prim : data->prim;
         uint64_t draw = kk_grid_is_indirect(draw_data.grid)
            ? draw_data.grid.addr : 0;
         if (!tess && data->restart && draw_data.index.el_size_B) {
            if (!draw) {
               VkDrawIndexedIndirectCommand direct = {
                  .indexCount = draw_data.grid.size.x,
                  .instanceCount = draw_data.grid.size.y,
               };
               draw = kk_pool_upload(cmd, &direct, sizeof(direct), 4).gpu;
            }
            libkk_primitives_generated_restart(cmd, kk_grid_1d(1024), true,
               gfx->primitives_query[0], draw, draw_data.index.gpu.addr,
               draw_data.index.gpu.range / draw_data.index.el_size_B,
               draw_data.index.el_size_B, data->restart_index, mode);
         } else {
            libkk_primitives_generated(cmd, kk_grid_1d(1), true,
               gfx->primitives_query[0], draw,
               draw_data.grid.size.x, draw_data.grid.size.y, mode);
         }
      }

      /* TODO_KOSMICKRISP Remove this once unroll, tess and any compute does not
       * split render pass */
      if (cmd->state.gfx.need_to_start_render_pass)
         kk_flush_gfx_state(cmd);

      if (gfx->render.view_mask) {
         uint32_t layers[KK_MAX_MULTIVIEW_VIEW_COUNT] = {0};
         uint32_t count = 0;
         if (tess || vs->info.api_geometry || gfx->layer_replay.bind ||
             gfx->poly_retained.consume ||
             (vs->info.poly_layer_replay && gfx->polygon_points)) {
            count = 1;
         } else {
            u_foreach_bit(view, gfx->render.view_mask)
               layers[count++] = view;
         }
         mtl_set_vertex_amplification_count(cs_get_render(cmd), layers, count);
      }
      if (cmd->interlock_heap_active) {
         VkResult result = kk_interlock_record_draw(cmd);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd->vk, result);
            goto layered_bc_done;
         }
      } else {
         if (unlikely(gfx->poly_replay_active))
            kk_poly_layer_dispatch_draw(cmd, draw_data);
         else
            kk_dispatch_draw(cmd, draw_data);
      }
      }
   }

layered_bc_done:
   kk_layered_bc_end_draw(cmd);
}

static void
kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   KK_PERF_ENCODER_SCOPE(kk_cmd_buffer_device(cmd), KK_PERF_ENCODER_DRAW, draw);
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *vs = cmd->state.gfx.shader;
   uint32_t view_mask = gfx->render.view_mask;
   if (!view_mask) {
      kk_draw_one_view(cmd, data);
      return;
   }

   bool cached_views = gfx->poly_retained.consume ||
      (vs->info.poly_layer_replay &&
       (gfx->render.layered_bc || kk_polygon_expansion_active(cmd) ||
        !vs->info.polygon_native));
   if (!(vs->info.api_geometry || cached_views ||
                      cmd->state.shaders[MESA_SHADER_TESS_EVAL])) {
      kk_draw_one_view(cmd, data);
      return;
   }

   /* Direct multi-draw commands have trailing records beyond draws[0].
    * Indirect records remain in GPU memory, so only their header is copied.
    * Reset the full snapshot for each view: predicate and topology lowering
    * can rewrite its draw representation. */
   size_t size = sizeof(*data);
   if (!data->indirect && data->draw_count > 1u) {
      size_t stride = data->indexed ? sizeof(data->indexed_draws[0])
                                    : sizeof(data->draws[0]);
      size += (size_t)(data->draw_count - 1u) * stride;
   }
   struct kk_draw_command header;
   struct kk_draw_command *per_view = size == sizeof(header)
      ? &header : ralloc_size(NULL, size);
   if (!per_view) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   /* As in Honeykrisp, replay actual work for each view. A one-bit mask keeps
    * every real query producer at one view per replay; EndQuery publishes the
    * summed first query plus the reset zero entries for the other views. */
   u_foreach_bit(view, view_mask) {
      gfx->render.view_mask = BITFIELD_BIT(view);
      gfx->per_draw_data.multiview_index = view;
      memcpy(per_view, data, size);
      kk_draw_one_view(cmd, per_view);
   }
   gfx->render.view_mask = view_mask;
   if (per_view != &header)
      ralloc_free(per_view);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount,
           uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
   /* Metal validation dislikes empty calls */
   if (instanceCount == 0 || vertexCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .draw_count = 1,
      .predicate_addr[0] = cmd->state.cond_render.address,
      .draws[0] = {
         .vertexCount = vertexCount,
         .instanceCount = instanceCount,
         .firstVertex = firstVertex,
         .firstInstance = firstInstance,
      }};

   kk_draw(cmd, &data);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount,
                   const VkMultiDrawInfoEXT *pVertexInfo,
                   uint32_t instanceCount, uint32_t firstInstance,
                   uint32_t stride)
{
   /* Metal validation dislikes empty calls */
   if (drawCount == 0 || instanceCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   /* Build final draw list from parameters */
   struct kk_draw_command *data =
      rzalloc_size(NULL, sizeof(struct kk_draw_command) +
                            sizeof(VkDrawIndirectCommand) * (drawCount - 1u));
   if (!data) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   *data = (struct kk_draw_command){
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .predicate_addr[0] = cmd->state.cond_render.address,
   };

   for (uint32_t i = 0; i < drawCount; ++i) {
      /* Metal validation dislikes empty calls */
      if (pVertexInfo->vertexCount > 0) {
         data->draws[data->draw_count] = (VkDrawIndirectCommand){
            .vertexCount = pVertexInfo->vertexCount,
            .instanceCount = instanceCount,
            .firstVertex = pVertexInfo->firstVertex,
            .firstInstance = firstInstance,
         };
         data->draw_count += 1u;
      }

      pVertexInfo = ((void *)pVertexInfo) + stride;
   }

   if (data->draw_count > 0)
      kk_draw(cmd, data);

   ralloc_free(data);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                  uint32_t instanceCount, uint32_t firstIndex,
                  int32_t vertexOffset, uint32_t firstInstance)
{
   /* Metal validation dislikes empty calls */
   if (instanceCount == 0 || indexCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .index_buffer = cmd->state.gfx.index.gpu,
      .index_buffer_el_size_B = cmd->state.gfx.index.bytes_per_index,
      .indexed = true,
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .draw_count = 1,
      .predicate_addr[0] = cmd->state.cond_render.address,
      .indexed_draws[0] =
         {
            .indexCount = indexCount,
            .instanceCount = instanceCount,
            .firstIndex = firstIndex,
            .vertexOffset = vertexOffset,
            .firstInstance = firstInstance,
         },
   };

   kk_draw(cmd, &data);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawMultiIndexedEXT(VkCommandBuffer commandBuffer, uint32_t drawCount,
                          const VkMultiDrawIndexedInfoEXT *pIndexInfo,
                          uint32_t instanceCount, uint32_t firstInstance,
                          uint32_t stride, const int32_t *pVertexOffset)
{
   /* Metal validation dislikes empty calls */
   if (drawCount == 0 || instanceCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   /* Build final draw list from parameters */
   struct kk_draw_command *data = ralloc_size(
      NULL, sizeof(struct kk_draw_command) +
               sizeof(VkDrawIndexedIndirectCommand) * (drawCount - 1u));
   if (!data) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   *data = (struct kk_draw_command){
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .index_buffer = cmd->state.gfx.index.gpu,
      .index_buffer_el_size_B = cmd->state.gfx.index.bytes_per_index,
      .indexed = true,
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .predicate_addr[0] = cmd->state.cond_render.address,
   };

   for (uint32_t i = 0; i < drawCount; ++i) {
      /* Metal validation dislikes empty calls */
      if (pIndexInfo->indexCount > 0) {
         data->indexed_draws[data->draw_count] = (VkDrawIndexedIndirectCommand){
            .indexCount = pIndexInfo->indexCount,
            .instanceCount = instanceCount,
            .firstIndex = pIndexInfo->firstIndex,
            .vertexOffset = pVertexOffset != NULL ? *pVertexOffset
                                                  : pIndexInfo->vertexOffset,
            .firstInstance = firstInstance,
         };
         data->draw_count += 1u;
      }

      pIndexInfo = ((void *)pIndexInfo) + stride;
   }

   if (data->draw_count > 0)
      kk_draw(cmd, data);

   ralloc_free(data);
}

#include "kk_dgc_poly.h"
#include "kk_dgc_indirect.h"

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndirect2KHR(VkCommandBuffer commandBuffer,
                       const VkDrawIndirect2InfoKHR *pInfo)
{
   if (pInfo->drawCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .indirect = true,
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .draw_count = pInfo->drawCount,
      .predicate_addr[0] = cmd->state.cond_render.address,
      .indirect_command.addr = pInfo->addressRange.address,
      .indirect_command.stride = pInfo->addressRange.stride,
   };

   if (data.draw_count > 4096)
      kk_draw_large_indirect(cmd, &data);
   else
      kk_draw(cmd, &data);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndexedIndirect2KHR(VkCommandBuffer commandBuffer,
                              const VkDrawIndirect2InfoKHR *pInfo)
{
   if (pInfo->drawCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .index_buffer = cmd->state.gfx.index.gpu,
      .index_buffer_el_size_B = cmd->state.gfx.index.bytes_per_index,
      .indirect = true,
      .indexed = true,
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .draw_count = pInfo->drawCount,
      .predicate_addr[0] = cmd->state.cond_render.address,
      .indirect_command.addr = pInfo->addressRange.address,
      .indirect_command.stride = pInfo->addressRange.stride,
   };

   if (data.draw_count > 4096)
      kk_draw_large_indirect(cmd, &data);
   else
      kk_draw(cmd, &data);
}

/* Execution-set variants use the exact selected ordinary cache and DS path. */
void kk_dgc_selected_graphics(struct kk_cmd_buffer *cmd,
   mtl_render_pipeline_state **render, mtl_depth_stencil_state **depth)
{
   struct kk_shader *shader = cmd->state.gfx.shader;
   *render = kk_attachment_pipeline(cmd, shader, cmd->state.gfx.fsr_coarse ?
      KK_PIPELINE_FSR_COARSE : KK_PIPELINE_MAIN);
   *depth = kk_cmd_selected_depth_stencil(cmd, shader);
}

/* DGC uses the same pipeline, attachment and dynamic state flush as draws. */
void kk_dgc_flush_graphics(struct kk_cmd_buffer *cmd) { kk_flush_gfx_state(cmd); }
bool kk_dgc_prepare_graphics_root(struct kk_cmd_buffer *allocator,
   struct kk_cmd_buffer *state, struct kk_root_descriptor_table *root)
{
   if (state->state.shaders[MESA_SHADER_MESH]) return true;
   return kk_prepare_vertex_input(allocator, &state->state.gfx,
      &state->vk.dynamic_graphics_state, state->state.shaders[MESA_SHADER_VERTEX], root);
}
uint32_t kk_dgc_primitive(struct kk_cmd_buffer *cmd) { return mesa_prim_to_mtl_primitive_type(vk_topology_to_mesa(cmd->vk.dynamic_graphics_state.ia.primitive_topology)); }

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndirectCount2KHR(VkCommandBuffer commandBuffer,
                            const VkDrawIndirectCount2InfoKHR *pInfo)
{
   if (pInfo->maxDrawCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   if (!cmd->state.gfx.render.layered_bc && pInfo->maxDrawCount >= 1024 &&
       !cmd->state.shaders[MESA_SHADER_TESS_EVAL] && !kk_geometry_shader(cmd) &&
       cmd->vk.dynamic_graphics_state.rs.polygon_mode == VK_POLYGON_MODE_FILL &&
       !cmd->state.gfx.xfb_enabled && !cmd->state.pipeline_statistics &&
       !cmd->state.gfx.primitives_query[0] &&
       (!false || (cmd->state.gfx.index.bytes_per_index >= 2 &&
        !cmd->vk.dynamic_graphics_state.ia.primitive_restart_enable)) &&
       !mesa_prim_has_adjacency(vk_topology_to_mesa(cmd->vk.dynamic_graphics_state.ia.primitive_topology)) &&
       cmd->vk.dynamic_graphics_state.ia.primitive_topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
      kk_dgc_draw_indirect_count(cmd,pInfo,false);
      return;
   }

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .indirect = true,
      .predicate_count = cmd->state.cond_render.enabled ? 2u : 1u,
      .predicate_op[0] = KK_PREDICATE_GT_DRAW_ID,
      .predicate_op[1] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .draw_count = pInfo->maxDrawCount,
      .predicate_addr[0] = pInfo->countAddressRange.address,
      .predicate_addr[1] = cmd->state.cond_render.address,
      .indirect_command.addr = pInfo->addressRange.address,
      .indirect_command.stride = pInfo->addressRange.stride,
   };

   if (data.draw_count > 4096)
      kk_draw_large_indirect(cmd, &data);
   else
      kk_draw(cmd, &data);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndexedIndirectCount2KHR(VkCommandBuffer commandBuffer,
                                   const VkDrawIndirectCount2InfoKHR *pInfo)
{
   if (pInfo->maxDrawCount == 0)
      return;

   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);

   if (!cmd->state.gfx.render.layered_bc && pInfo->maxDrawCount >= 1024 &&
       !cmd->state.shaders[MESA_SHADER_TESS_EVAL] && !kk_geometry_shader(cmd) &&
       cmd->vk.dynamic_graphics_state.rs.polygon_mode == VK_POLYGON_MODE_FILL &&
       !cmd->state.gfx.xfb_enabled && !cmd->state.pipeline_statistics &&
       !cmd->state.gfx.primitives_query[0] &&
       (!true || (cmd->state.gfx.index.bytes_per_index >= 2 &&
        !cmd->vk.dynamic_graphics_state.ia.primitive_restart_enable)) &&
       !mesa_prim_has_adjacency(vk_topology_to_mesa(cmd->vk.dynamic_graphics_state.ia.primitive_topology)) &&
       cmd->vk.dynamic_graphics_state.ia.primitive_topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
      kk_dgc_draw_indirect_count(cmd,pInfo,true);
      return;
   }

   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .index_buffer = cmd->state.gfx.index.gpu,
      .index_buffer_el_size_B = cmd->state.gfx.index.bytes_per_index,
      .indirect = true,
      .indexed = true,
      .predicate_count = cmd->state.cond_render.enabled ? 2u : 1u,
      .predicate_op[0] = KK_PREDICATE_GT_DRAW_ID,
      .predicate_op[1] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .draw_count = pInfo->maxDrawCount,
      .predicate_addr[0] = pInfo->countAddressRange.address,
      .predicate_addr[1] = cmd->state.cond_render.address,
      .indirect_command.addr = pInfo->addressRange.address,
      .indirect_command.stride = pInfo->addressRange.stride,
   };

   if (data.draw_count > 4096)
      kk_draw_large_indirect(cmd, &data);
   else
      kk_draw(cmd, &data);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindTransformFeedbackBuffers2EXT(VkCommandBuffer commandBuffer,
   uint32_t firstBinding, uint32_t bindingCount,
   const VkBindTransformFeedbackBuffer2InfoEXT *bindings)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   for (unsigned i = 0; i < bindingCount; ++i) {
      cmd->state.gfx.xfb[firstBinding + i] = (struct kk_addr_range){
         .addr = bindings[i].addressRange.address,
         .range = bindings[i].addressRange.size,
      };
   }
}

static void
kk_begin_end_xfb(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBindTransformFeedbackBuffer2InfoEXT *buffers, bool begin)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   gfx->xfb_enabled = begin;
   if (!gfx->xfb_offsets)
      gfx->xfb_offsets = kk_pool_alloc(cmd, POLY_MAX_SO_BUFFERS * 4, 4).gpu;
   for (unsigned i = 0; i < (begin ? POLY_MAX_SO_BUFFERS : count); ++i) {
      unsigned slot = begin ? i : first + i;
      bool selected = slot >= first && slot - first < count && buffers &&
                      buffers[slot - first].addressRange.address != 0;
      uint64_t source = 0, destination = gfx->xfb_offsets + slot * 4;
      if (selected) {
         source = buffers[slot - first].addressRange.address;
      }
      if (!begin) {
         if (!selected) continue;
         uint64_t tmp = destination; destination = source; source = tmp;
      }
      libkk_xfb_counter_copy(cmd, kk_grid_1d(1), true, source, destination);
   }
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginTransformFeedback2EXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBindTransformFeedbackBuffer2InfoEXT *buffers)
{
   kk_begin_end_xfb(commandBuffer, first, count, buffers, true);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdEndTransformFeedback2EXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBindTransformFeedbackBuffer2InfoEXT *buffers)
{
   kk_begin_end_xfb(commandBuffer, first, count, buffers, false);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndirectByteCount2EXT(VkCommandBuffer commandBuffer,
   uint32_t instanceCount, uint32_t firstInstance,
   const VkBindTransformFeedbackBuffer2InfoEXT *counter,
   uint32_t counterOffset, uint32_t vertexStride)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   uint64_t draw = kk_pool_alloc(cmd, sizeof(VkDrawIndirectCommand), 4).gpu;
   libkk_xfb_draw(cmd, kk_grid_1d(1), true,
      counter->addressRange.address, draw,
      instanceCount, firstInstance, counterOffset, vertexStride);
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .draw_count = 1, .indirect = true,
      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,
      .predicate_op[0] = cmd->state.cond_render.inverted
                            ? KK_PREDICATE_EQ_ZERO
                            : KK_PREDICATE_NEQ_ZERO,
      .predicate_addr[0] = cmd->state.cond_render.address,
      .indirect_command = { .addr = draw, .stride = sizeof(VkDrawIndirectCommand) },
   };
   kk_draw(cmd, &data);
}

#include "kk_msrtss.h"

void
kk_init_rendering_locations(struct kk_cmd_buffer *cmd, const VkRenderingInfo *pRenderingInfo)
{
   const VkRenderingAttachmentLocationInfoKHR ral_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR,
      .colorAttachmentCount = pRenderingInfo->colorAttachmentCount,
   };
   vk_cmd_set_rendering_attachment_locations(&cmd->vk, &ral_info);
}
