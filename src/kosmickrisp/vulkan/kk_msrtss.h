// SPDX-License-Identifier: MIT
#include "kk_msrtss_state.h"
// Legacy subpasses reach this same path through Mesa's render-pass lowering.
#include "vk_meta.h"

static bool
kk_msrtss_same_view(const struct kk_image_view *a,
                    const struct kk_image_view *b)
{
   return a->vk.image == b->vk.image &&
          a->vk.view_format == b->vk.view_format &&
          a->vk.base_mip_level == b->vk.base_mip_level &&
          a->vk.base_array_layer == b->vk.base_array_layer &&
          a->vk.layer_count == b->vk.layer_count;
}

/* A completely classified pass can resolve directly from tile storage. A
 * LOAD or an encoder break still needs the persistent multisample backing. */
static bool
kk_msrtss_tile_safe(struct kk_cmd_buffer *cmd,
                    const struct kk_image_view *src,
                    const VkRenderingInfo *info,
                    const VkRenderingAttachmentInfo *att)
{
   /* The completed command list rejects actual split-producing pipelines.
    * Device feature enablement alone does not make a native pass unsafe. */
   if (!cmd->transient_preflight_done || !cmd->transient_safe ||
       (info->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT)) ||
       (att->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
        att->loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE) ||
       !(src->vk.aspects & VK_IMAGE_ASPECT_COLOR_BIT) ||
       info->renderArea.offset.x || info->renderArea.offset.y ||
       info->renderArea.extent.width != src->vk.extent.width ||
       info->renderArea.extent.height != src->vk.extent.height)
      return false;
   const uint32_t layers = info->viewMask ? util_last_bit(info->viewMask) : info->layerCount;
   if (layers != src->vk.layer_count ||
       (info->viewMask && info->viewMask != BITFIELD_MASK(layers)))
      return false;
   if (att->storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
       (vk_format_is_int(src->vk.view_format) ||
        !kk_get_va_format(vk_format_to_pipe_format(src->vk.view_format))->resolve ||
        (vk_get_rendering_attachment_flags(att) &
         VK_RENDERING_ATTACHMENT_RESOLVE_SKIP_TRANSFER_FUNCTION_BIT_KHR)))
      return false;
   /* Shared views must retain one backing even when their load operations
    * disagree. Leave that uncommon alias case on the existing private path. */
   for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *other = &info->pColorAttachments[i];
      VK_FROM_HANDLE(kk_image_view, view, other->imageView);
      if (view && ((other != att && kk_msrtss_same_view(src, view)) ||
          view->vk.extent.width != src->vk.extent.width ||
          view->vk.extent.height != src->vk.extent.height))
         return false;
   }
   const VkRenderingAttachmentInfo *ds[2] = {
      info->pDepthAttachment, info->pStencilAttachment,
   };
   for (uint32_t i = 0; i < 2; i++) {
      VK_FROM_HANDLE(kk_image_view, view, ds[i] ? ds[i]->imageView : VK_NULL_HANDLE);
      if (view && (view->vk.extent.width != src->vk.extent.width ||
                   view->vk.extent.height != src->vk.extent.height))
         return false;
   }
   return true;
}

static VkResult
kk_msrtss_image(struct kk_cmd_buffer *cmd, struct kk_image_view *src,
                VkSampleCountFlagBits samples, bool transient, VkImageView *view_out)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const VkDevice device = kk_device_to_handle(dev);
   util_dynarray_foreach(&cmd->msrtss_images, struct kk_msrtss_image, cached) {
      if (kk_msrtss_same_view(cached->source, src) &&
          cached->samples == samples && cached->transient == transient && cached->view) {
         *view_out = cached->view;
         return VK_SUCCESS;
      }
   }
   const VkImageAspectFlags aspects = vk_format_aspects(src->vk.view_format);
   VkExtent3D extent = vk_image_mip_level_extent(src->vk.image,
                                               src->vk.base_mip_level);
   extent.depth = 1;
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = src->vk.view_format,
      .extent = extent,
      .mipLevels = 1,
      .arrayLayers = src->vk.layer_count,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = (transient ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT :
                             VK_IMAGE_USAGE_SAMPLED_BIT) |
         ((aspects & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                               : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   struct kk_msrtss_image image = {
      .source = src, .samples = samples, .transient = transient,
   };
   VkResult result = kk_CreateImage(device, &image_info, NULL, &image.image);
   if (result != VK_SUCCESS)
      return result;
   const VkImageMemoryRequirementsInfo2 requirement_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = image.image,
   };
   VkMemoryRequirements2 requirement_result = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   kk_GetImageMemoryRequirements2(device, &requirement_info, &requirement_result);
   const VkMemoryRequirements requirements = requirement_result.memoryRequirements;
   struct kk_physical_device *pdev = kk_device_physical(dev);
   uint32_t memory_type = ffs(requirements.memoryTypeBits) - 1;
   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      if ((requirements.memoryTypeBits & BITFIELD_BIT(i)) &&
          !(pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         memory_type = i;
         break;
      }
   }
   if (transient) {
      for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
         if ((requirements.memoryTypeBits & BITFIELD_BIT(i)) &&
             (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
            memory_type = i;
            break;
         }
      }
   }
   const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type,
   };
   result = kk_AllocateMemory(device, &allocation, NULL, &image.memory);
   if (result != VK_SUCCESS) {
      kk_DestroyImage(device, image.image, NULL);
      return result;
   }
   const VkBindImageMemoryInfo bind = {
      .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image = image.image,
      .memory = image.memory,
   };
   result = kk_BindImageMemory2(device, 1, &bind);
   // Own even a failed bind until reset, along with any recorded work.
   util_dynarray_append(&cmd->msrtss_images, image);
   if (result != VK_SUCCESS)
      return result;
   /* Automatic internal sampling usage must not force the tile-only view
    * into private storage. This view is consumed only by the native pass. */
   const VkImageViewUsageCreateInfo view_usage = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   };
   const VkImageViewCreateInfo view = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = transient ? &view_usage : NULL,
      .flags = VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
      .image = image.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .format = src->vk.view_format,
      .subresourceRange = {aspects, 0, 1, 0, src->vk.layer_count},
   };
   result = vk_meta_create_image_view(&cmd->vk, &dev->meta, &view, view_out);
   if (result == VK_SUCCESS)
      util_dynarray_top_ptr(&cmd->msrtss_images, struct kk_msrtss_image)->view = *view_out;
   return result;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginRendering(VkCommandBuffer commandBuffer,
                     const VkRenderingInfo *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   const VkSampleCountFlagBits samples = kk_msrtss_samples(info);
   if (samples == VK_SAMPLE_COUNT_1_BIT) {
      kk_begin_rendering(commandBuffer, info);
      return;
   }

   VkRenderingInfo rendering = *info;
   VkRenderingAttachmentInfo colors[KK_MAX_RTS];
   VkRenderingAttachmentInfo depth = {0}, stencil = {0};
   VkRenderingAttachmentInfo *attachments[KK_MAX_RTS + 2];
   uint32_t count = 0;
   for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
      colors[i] = info->pColorAttachments[i];
      attachments[count++] = &colors[i];
   }
   rendering.pColorAttachments = colors;
   if (info->pDepthAttachment) {
      depth = *info->pDepthAttachment;
      rendering.pDepthAttachment = &depth;
      attachments[count++] = &depth;
   }
   if (info->pStencilAttachment) {
      stencil = *info->pStencilAttachment;
      rendering.pStencilAttachment = &stencil;
      attachments[count++] = &stencil;
   }

   struct kk_image_view *originals[KK_MAX_RTS + 2] = {0};
   VkImageView implicit[KK_MAX_RTS + 2] = {0};
   for (uint32_t i = 0; i < count; i++) {
      VkRenderingAttachmentInfo *att = attachments[i];
      VK_FROM_HANDLE(kk_image_view, src, att->imageView);
      if (!src || src->vk.image->samples != VK_SAMPLE_COUNT_1_BIT)
         continue;
      originals[i] = src;
      for (uint32_t j = 0; j < i; j++) {
         if (originals[j] && kk_msrtss_same_view(src, originals[j])) {
            implicit[i] = implicit[j];
            break;
         }
      }
      if (implicit[i] == VK_NULL_HANDLE) {
         const VkRenderingAttachmentInfo *original = i < info->colorAttachmentCount
            ? &info->pColorAttachments[i] : att;
         const bool transient = kk_msrtss_tile_safe(cmd, src, info, original);
         VkResult result = kk_msrtss_image(cmd, src,
            samples, transient, &implicit[i]);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd->vk, result);
            return;
         }
      }
      VkImageAspectFlags aspect = att == &depth ? VK_IMAGE_ASPECT_DEPTH_BIT :
         att == &stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      VK_FROM_HANDLE(kk_image_view, dst, implicit[i]);
      if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD ||
          (info->flags & VK_RENDERING_RESUMING_BIT))
         kk_meta_expand_msrtss(cmd, src, vk_image_to_handle(dst->vk.image), aspect);

      const bool needs_resolve = att->storeOp == VK_ATTACHMENT_STORE_OP_STORE ||
                                 (info->flags & VK_RENDERING_SUSPENDING_BIT);
      att->resolveImageView = needs_resolve ? att->imageView : VK_NULL_HANDLE;
      att->resolveImageLayout = att->imageLayout;
      att->imageView = implicit[i];
      if (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
         att->resolveMode = vk_format_is_int(src->vk.view_format)
            ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
      else if (att->resolveMode == VK_RESOLVE_MODE_NONE)
         att->resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      if (!needs_resolve)
         att->resolveMode = VK_RESOLVE_MODE_NONE;
      // Only intermediate encoder breaks need to preserve multisample storage.
      att->storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   }
   kk_begin_rendering(commandBuffer, &rendering);
   cmd->state.gfx.render.msrtss = true;
}
