// SPDX-License-Identifier: MIT
/* Lazy images use tile storage until a pass requires persistent backing. */
#ifndef KK_TRANSIENT_H
#define KK_TRANSIENT_H
#include "kk_cmd_buffer.h"
#include "kk_image_view.h"
#include "vk_format.h"
#include "vk_render_pass.h"

bool
kk_image_plane_is_committed(const struct kk_image_plane *plane)
{
   if (!plane->memoryless)
      return true;
   simple_mtx_lock(&plane->mem->transient_mutex);
   bool committed = plane->mem->bo != NULL;
   simple_mtx_unlock(&plane->mem->transient_mutex);
   return committed;
}

mtl_texture *
kk_image_plane_render_base(const struct kk_image_plane *plane)
{
   if (!plane->memoryless)
      return plane->mtl_handle;
   simple_mtx_lock(&plane->mem->transient_mutex);
   mtl_texture *texture = plane->private_handle ?: plane->mtl_handle;
   simple_mtx_unlock(&plane->mem->transient_mutex);
   return texture;
}

/* Caller holds the memory mutex. Both native textures live with the image. */
static VkResult
kk_transient_private_locked(struct kk_device *dev, struct kk_image_plane *plane)
{
   VkResult result = kk_device_memory_commit_private(dev, plane->mem);
   if (result != VK_SUCCESS || plane->private_handle)
      return result;
   plane->private_handle = kk_image_plane_create_texture(plane, &plane->layout, 0);
   return plane->private_handle ? VK_SUCCESS :
      vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

VkResult
kk_image_plane_require_private(struct kk_device *dev, struct kk_image_plane *plane)
{
   simple_mtx_lock(&plane->mem->transient_mutex);
   VkResult result = kk_transient_private_locked(dev, plane);
   simple_mtx_unlock(&plane->mem->transient_mutex);
   return result;
}

static VkResult
kk_transient_private_view(struct kk_device *dev, struct kk_image_view *view)
{
   struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   struct kk_image_plane *plane = &image->planes[0];
   if (!plane->memoryless)
      return VK_SUCCESS;
   simple_mtx_lock(&plane->mem->transient_mutex);
   VkResult result = kk_transient_private_locked(dev, plane);
   if (result == VK_SUCCESS && !view->planes[0].transient_private) {
      if (!view->planes[0].render_is_view) {
         view->planes[0].transient_private = mtl_retain(plane->private_handle);
      } else {
         const struct kk_va_format *format = kk_get_va_format(view->planes[0].format);
         const struct kk_view_layout layout = {
            .view_type = view->vk.view_type,
            .sample_count_sa = image->vk.samples,
            .format = {.pipe = view->planes[0].format, .mtl = format->mtl_pixel_format},
            .base_level = view->vk.base_mip_level,
            .num_levels = view->vk.level_count,
            .base_array_layer = view->vk.base_array_layer,
            .array_len = view->vk.layer_count,
         };
         view->planes[0].transient_private =
            mtl_new_texture_view_with_no_swizzle(plane->private_handle, &layout);
      }
      if (!view->planes[0].transient_private)
         result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   simple_mtx_unlock(&plane->mem->transient_mutex);
   return result;
}

static bool
kk_transient_attachment_safe(struct kk_cmd_buffer *cmd,
                             const VkRenderingInfo *rendering,
                             const VkRenderingAttachmentInfo *info,
                             VkImageAspectFlagBits aspect)
{
   VK_FROM_HANDLE(kk_image_view, view, info->imageView);
   struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   const struct kk_image_plane *plane = &image->planes[0];
   const uint32_t layers = rendering->viewMask
      ? util_last_bit(rendering->viewMask) : rendering->layerCount;
   simple_mtx_lock(&plane->mem->transient_mutex);
   const bool uncommitted = !plane->mem->bo && plane->mem->image_bind_count == 1;
   simple_mtx_unlock(&plane->mem->transient_mutex);
   if (!cmd->transient_safe || !uncommitted ||
       (info->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
        info->loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE) ||
       (info->storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE &&
        info->storeOp != VK_ATTACHMENT_STORE_OP_NONE) ||
       rendering->renderArea.offset.x || rendering->renderArea.offset.y ||
       rendering->renderArea.extent.width != view->vk.extent.width ||
       rendering->renderArea.extent.height != view->vk.extent.height ||
       layers != view->vk.layer_count ||
       (rendering->viewMask && rendering->viewMask != BITFIELD_MASK(layers)))
      return false;
   if (info->resolveMode == VK_RESOLVE_MODE_NONE)
      return true;
   VK_FROM_HANDLE(kk_image_view, resolve, info->resolveImageView);
   const struct kk_attachment attachment = {
      .flags = vk_get_rendering_attachment_flags(info),
      .vk_format = view->vk.format, .iview = view,
      .resolve_mode = info->resolveMode, .resolve_iview = resolve,
   };
   return kk_attachment_do_renderpass_resolve(&attachment, true, aspect);
}

VkResult
kk_transient_prepare_rendering(struct kk_cmd_buffer *cmd,
                               const VkRenderingInfo *rendering)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const VkRenderingAttachmentInfo *attachments[KK_MAX_RTS + 2];
   VkImageAspectFlagBits aspects[KK_MAX_RTS + 2];
   uint32_t count = 0;
   for (uint32_t i = 0; i < rendering->colorAttachmentCount; i++) {
      attachments[count] = &rendering->pColorAttachments[i];
      aspects[count++] = VK_IMAGE_ASPECT_COLOR_BIT;
   }
   if (rendering->pDepthAttachment) {
      attachments[count] = rendering->pDepthAttachment;
      aspects[count++] = VK_IMAGE_ASPECT_DEPTH_BIT;
   }
   if (rendering->pStencilAttachment) {
      attachments[count] = rendering->pStencilAttachment;
      aspects[count++] = VK_IMAGE_ASPECT_STENCIL_BIT;
   }
   for (uint32_t i = 0; i < count; i++) {
      const VkRenderingAttachmentInfo *info = attachments[i];
      if (info->imageView == VK_NULL_HANDLE)
         continue;
      VK_FROM_HANDLE(kk_image_view, view, info->imageView);
      struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
      if (image->planes[0].memoryless &&
          !kk_transient_attachment_safe(cmd, rendering, info, aspects[i])) {
         VkResult result = kk_transient_private_view(dev, view);
         if (result != VK_SUCCESS)
            return result;
      }
      if (info->resolveMode != VK_RESOLVE_MODE_NONE) {
         VK_FROM_HANDLE(kk_image_view, resolve, info->resolveImageView);
         VkResult result = kk_transient_private_view(dev, resolve);
         if (result != VK_SUCCESS)
            return result;
      }
   }
   /* An aliased or shared depth/stencil aspect can commit the same allocation. */
   for (uint32_t i = 0; i < count; i++) {
      if (attachments[i]->imageView == VK_NULL_HANDLE)
         continue;
      VK_FROM_HANDLE(kk_image_view, view, attachments[i]->imageView);
      struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
      if (image->planes[0].memoryless && kk_image_plane_is_committed(&image->planes[0])) {
         VkResult result = kk_transient_private_view(dev, view);
         if (result != VK_SUCCESS)
            return result;
      }
   }
   return VK_SUCCESS;
}

mtl_texture *
kk_transient_render_texture(const struct kk_image_view *view)
{
   const struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   const struct kk_image_plane *plane = &image->planes[0];
   if (!plane->memoryless)
      return view->planes[0].mtl_handle_render;
   simple_mtx_lock(&plane->mem->transient_mutex);
   mtl_texture *texture = view->planes[0].transient_private ?:
      view->planes[0].mtl_handle_render;
   simple_mtx_unlock(&plane->mem->transient_mutex);
   return texture;
}
#endif
