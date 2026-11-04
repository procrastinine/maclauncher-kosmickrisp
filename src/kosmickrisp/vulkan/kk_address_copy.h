/* SPDX-License-Identifier: MIT */
#include "vk_format.h"

/* Metal image copies require an MTLBuffer. Stage only the copied texel rows,
 * using the shared address-copy shader and the existing image-copy commands.
 * Row padding is neither read nor written. Each allocation is bounded; arrays
 * and volumes are streamed in bands with command-buffer lifetime. */
static void
kk_copy_address_image(VkCommandBuffer commandBuffer,
                      const VkCopyDeviceMemoryImageInfoKHR *info, bool to_image)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(kk_image, image, info->image);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const VkAddressCommandFlagsKHR staging_flags =
      VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR |
      VK_ADDRESS_COMMAND_UNKNOWN_STORAGE_BUFFER_USAGE_BIT_KHR;

   for (uint32_t i = 0; i < info->regionCount; ++i) {
      const VkDeviceMemoryImageCopyKHR *region = &info->pRegions[i];
      const struct vk_image_buffer_layout layout =
         vk_image_memory_copy_layout(&image->vk, region);
      const VkFormat format = vk_format_get_aspect_format(
         image->vk.format, region->imageSubresource.aspectMask);
      const struct util_format_description *fmt = vk_format_description(format);
      const VkExtent3D extent = vk_image_sanitize_extent(&image->vk,
                                                       region->imageExtent);
      const uint32_t row_bytes = DIV_ROUND_UP(extent.width, fmt->block.width) *
                                 layout.element_size_B;
      const uint32_t rows = DIV_ROUND_UP(extent.height, fmt->block.height);
      const uint32_t band_rows = MAX2(1, (16 * 1024 * 1024) / row_bytes);
      const uint32_t layers = vk_image_subresource_layer_count(
         &image->vk, &region->imageSubresource);

      for (uint32_t layer = 0; layer < layers; ++layer) {
         for (uint32_t z = 0; z < extent.depth; ++z) {
            for (uint32_t y = 0; y < rows; y += band_rows) {
               const uint32_t count = MIN2(band_rows, rows - y);
               const VkBufferCreateInfo buffer_info = {
                  .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                  .size = (VkDeviceSize)count * row_bytes,
                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               };
               VkBuffer staging;
               VkResult result = vk_meta_create_buffer(&cmd->vk, &dev->meta,
                                                        &buffer_info, &staging);
               if (result != VK_SUCCESS) {
                  vk_command_buffer_set_error(&cmd->vk, result);
                  return;
               }
               void *unused;
               result = dev->meta.cmd_bind_map_buffer(&cmd->vk, &dev->meta,
                                                       staging, &unused);
               if (result != VK_SUCCESS) {
                  vk_command_buffer_set_error(&cmd->vk, result);
                  return;
               }
               VK_FROM_HANDLE(kk_buffer, buffer, staging);
               const VkBufferImageCopy2 copy_region = {
                  .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                  .imageSubresource = {
                     .aspectMask = region->imageSubresource.aspectMask,
                     .mipLevel = region->imageSubresource.mipLevel,
                     .baseArrayLayer = region->imageSubresource.baseArrayLayer + layer,
                     .layerCount = 1,
                  },
                  .imageOffset = {
                     region->imageOffset.x,
                     region->imageOffset.y + y * fmt->block.height,
                     region->imageOffset.z + z,
                  },
                  .imageExtent = {
                     extent.width,
                     MIN2(count * fmt->block.height,
                          extent.height - y * fmt->block.height), 1,
                  },
               };
               if (!to_image) {
                  const VkCopyImageToBufferInfo2 copy = {
                     .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                     .srcImage = info->image,
                     .srcImageLayout = region->imageLayout,
                     .dstBuffer = staging,
                     .regionCount = 1, .pRegions = &copy_region,
                  };
                  kk_CmdCopyImageToBuffer2(commandBuffer, &copy);
               }
               /* Order the staging writes before their consumer. This is an
                * internal dependency, including when the caller uses a queue
                * whose public capability is transfer only. */
               if (!to_image)
                  KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, cs_get_compute(cmd),
                     MTL_STAGE_DISPATCH | MTL_STAGE_BLIT,
                     MTL_STAGE_DISPATCH | MTL_STAGE_BLIT, MTL_VISIBILITY_RESOURCE_ALIAS);
               for (uint32_t row = 0; row < count; ++row) {
                  const VkDeviceAddressRangeKHR address = {
                     .address = region->addressRange.address +
                        (layer + z) * layout.image_stride_B +
                        (uint64_t)(y + row) * layout.row_stride_B,
                     .size = row_bytes,
                  };
                  const VkDeviceAddressRangeKHR temporary = {
                     .address = vk_buffer_address(&buffer->vk,
                                                   (uint64_t)row * row_bytes),
                     .size = row_bytes,
                  };
                  const VkDeviceMemoryCopyKHR copy_region = {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
                     .srcRange = to_image ? address : temporary,
                     .srcFlags = to_image ? region->addressFlags : staging_flags,
                     .dstRange = to_image ? temporary : address,
                     .dstFlags = to_image ? staging_flags : region->addressFlags,
                  };
                  const VkCopyDeviceMemoryInfoKHR copy = {
                     .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
                     .regionCount = 1, .pRegions = &copy_region,
                  };
                  kk_CmdCopyMemoryKHR(commandBuffer, &copy);
               }
               if (to_image) {
                  KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, cs_get_compute(cmd),
                     MTL_STAGE_DISPATCH | MTL_STAGE_BLIT,
                     MTL_STAGE_DISPATCH | MTL_STAGE_BLIT, MTL_VISIBILITY_RESOURCE_ALIAS);
                  const VkCopyBufferToImageInfo2 copy = {
                     .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                     .srcBuffer = staging,
                     .dstImage = info->image,
                     .dstImageLayout = region->imageLayout,
                     .regionCount = 1, .pRegions = &copy_region,
                  };
                  kk_CmdCopyBufferToImage2(commandBuffer, &copy);
               }
            }
         }
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdCopyMemoryToImageKHR(VkCommandBuffer commandBuffer,
                          const VkCopyDeviceMemoryImageInfoKHR *info)
{
   kk_copy_address_image(commandBuffer, info, true);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdCopyImageToMemoryKHR(VkCommandBuffer commandBuffer,
                          const VkCopyDeviceMemoryImageInfoKHR *info)
{
   kk_copy_address_image(commandBuffer, info, false);
}
