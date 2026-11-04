/* SPDX-License-Identifier: MIT */
#include "kk_sparse.h"

/* A meta push-descriptor layout may grow/reallocate its scratch set. Keep the
 * caller's set outside that allocation for the duration of this operation;
 * the existing meta helper still owns all other pipeline/state save/restore. */
struct kk_standard_3d_meta_save {
   struct kk_meta_save common;
   struct kk_push_descriptor_set *push;
};

static void
kk_standard_3d_meta_begin(struct kk_cmd_buffer *cmd,
   struct kk_standard_3d_meta_save *save,const char *label)
{
   struct kk_descriptor_state *desc=kk_get_descriptors_state(cmd,VK_PIPELINE_BIND_POINT_COMPUTE);
   save->push=desc->push[0];desc->push[0]=NULL;
   kk_meta_begin(cmd,&save->common,VK_PIPELINE_BIND_POINT_COMPUTE,label);
}

static void
kk_standard_3d_meta_end(struct kk_cmd_buffer *cmd,
   struct kk_standard_3d_meta_save *save)
{
   struct kk_descriptor_state *desc=kk_get_descriptors_state(cmd,VK_PIPELINE_BIND_POINT_COMPUTE);
   kk_meta_end(cmd,&save->common,VK_PIPELINE_BIND_POINT_COMPUTE);
   vk_free(&cmd->vk.pool->alloc,desc->push[0]);
   desc->push[0]=save->push;
   if(save->push)desc->push_dirty|=BITFIELD_BIT(0);
}



static struct vk_meta_copy_image_properties
kk_standard_3d_copy_properties(const struct kk_image *image)
{
   VkFormat format;
   switch (util_format_get_blocksize(vk_format_to_pipe_format(image->vk.format))) {
   case 1: format = VK_FORMAT_R8_UINT; break;
   case 2: format = VK_FORMAT_R16_UINT; break;
   case 4: format = VK_FORMAT_R32_UINT; break;
   case 8: format = VK_FORMAT_R32G32_UINT; break;
   case 16: format = VK_FORMAT_R32G32B32A32_UINT; break;
   default: UNREACHABLE("non power-of-two sparse volume format");
   }
   return (struct vk_meta_copy_image_properties){
      .color.view_format = format, .tile_size = {8,8,1},
   };
}

void
kk_standard_3d_copy_buffer_to_image(struct kk_cmd_buffer *cmd,
   const VkCopyBufferToImageInfo2 *info)
{
   VK_FROM_HANDLE(kk_image, image, info->dstImage);
   assert(kk_image_is_standard_3d(image));
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_standard_3d_meta_save save;
   kk_standard_3d_meta_begin(cmd, &save, "meta:sparse volume upload");
   const struct vk_meta_copy_image_properties props = kk_standard_3d_copy_properties(image);
   vk_meta_copy_buffer_to_image(&cmd->vk, &dev->meta, info, &props, VK_PIPELINE_BIND_POINT_COMPUTE);
   kk_standard_3d_meta_end(cmd, &save);
}

void
kk_standard_3d_copy_image_to_buffer(struct kk_cmd_buffer *cmd,
   const VkCopyImageToBufferInfo2 *info)
{
   VK_FROM_HANDLE(kk_image, image, info->srcImage);
   assert(kk_image_is_standard_3d(image));
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_standard_3d_meta_save save;
   kk_standard_3d_meta_begin(cmd, &save, "meta:sparse volume download");
   const struct vk_meta_copy_image_properties props = kk_standard_3d_copy_properties(image);
   vk_meta_copy_image_to_buffer(&cmd->vk, &dev->meta, info, &props);
   kk_standard_3d_meta_end(cmd, &save);
}

/* An ordinary compressed image may have an optimized, opaque mip layout.
 * Its raw block view cannot be reconstructed from byte offsets. Keep the
 * ordinary side on native image/buffer copies and the canonical side on the
 * existing page translator, with one reusable staging buffer per region. */
static void
kk_standard_2d_copy_compressed(struct kk_cmd_buffer *cmd,
                              const VkCopyImageInfo2 *info)
{
   VK_FROM_HANDLE(kk_image, src, info->srcImage);
   VK_FROM_HANDLE(kk_image, dst, info->dstImage);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const enum pipe_format sf = vk_format_to_pipe_format(src->vk.format);
   const enum pipe_format df = vk_format_to_pipe_format(dst->vk.format);
   for (uint32_t r = 0; r < info->regionCount; r++) {
      const VkImageCopy2 *region = &info->pRegions[r];
      VkExtent3D src_extent = vk_image_sanitize_extent(&src->vk, region->extent);
      VkExtent3D blocks = vk_image_extent_to_elements(&src->vk, src_extent);
      VkExtent3D dst_extent = {
         MIN2(blocks.width * util_format_get_blockwidth(df),
              u_minify(dst->vk.extent.width, region->dstSubresource.mipLevel) - region->dstOffset.x),
         MIN2(blocks.height * util_format_get_blockheight(df),
              u_minify(dst->vk.extent.height, region->dstSubresource.mipLevel) - region->dstOffset.y),
         1,
      };
      const VkBufferCreateInfo create = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = (uint64_t)blocks.width * blocks.height * util_format_get_blocksize(sf),
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         .queueFamilyIndexCount = 1,
         .pQueueFamilyIndices = &cmd->vk.pool->queue_family_index,
      };
      VkBuffer staging;
      VkResult result = vk_meta_create_buffer(&cmd->vk, &dev->meta, &create, &staging);
      void *map;
      if (result == VK_SUCCESS)
         result = dev->meta.cmd_bind_map_buffer(&cmd->vk, &dev->meta, staging, &map);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }
      const uint32_t layers = vk_image_subresource_layer_count(&src->vk, &region->srcSubresource);
      for (uint32_t layer = 0; layer < layers; layer++) {
         VkBufferImageCopy2 from = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .imageSubresource = region->srcSubresource,
            .imageOffset = region->srcOffset, .imageExtent = src_extent,
         };
         from.imageSubresource.baseArrayLayer += layer;
         from.imageSubresource.layerCount = 1;
         VkBufferImageCopy2 to = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .imageSubresource = region->dstSubresource,
            .imageOffset = region->dstOffset, .imageExtent = dst_extent,
         };
         to.imageSubresource.baseArrayLayer += layer;
         to.imageSubresource.layerCount = 1;
         const VkCopyImageToBufferInfo2 download = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
            .srcImage = info->srcImage, .srcImageLayout = info->srcImageLayout,
            .dstBuffer = staging, .regionCount = 1, .pRegions = &from,
         };
         const VkCopyBufferToImageInfo2 upload = {
            .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
            .srcBuffer = staging, .dstImage = info->dstImage,
            .dstImageLayout = info->dstImageLayout, .regionCount = 1, .pRegions = &to,
         };
         if (layer)
            mtl_barrier_after_encoder_stages(cs_get_compute(cmd),
               MTL_STAGE_BLIT | MTL_STAGE_DISPATCH, MTL_STAGE_BLIT | MTL_STAGE_DISPATCH);
         kk_CmdCopyImageToBuffer2(kk_cmd_buffer_to_handle(cmd), &download);
         mtl_barrier_after_encoder_stages(cs_get_compute(cmd),
            MTL_STAGE_BLIT | MTL_STAGE_DISPATCH, MTL_STAGE_BLIT | MTL_STAGE_DISPATCH);
         kk_CmdCopyBufferToImage2(kk_cmd_buffer_to_handle(cmd), &upload);
      }
   }
}

void
kk_standard_3d_copy_image(struct kk_cmd_buffer *cmd, const VkCopyImageInfo2 *info)
{
   VK_FROM_HANDLE(kk_image, src, info->srcImage);
   VK_FROM_HANDLE(kk_image, dst, info->dstImage);
   assert(kk_image_is_standard_3d(src) || kk_image_is_standard_3d(dst));
   if (src->vk.image_type == VK_IMAGE_TYPE_2D && dst->vk.image_type == VK_IMAGE_TYPE_2D &&
       vk_format_is_compressed(src->vk.format) && vk_format_is_compressed(dst->vk.format) &&
       kk_image_is_standard_3d(src) != kk_image_is_standard_3d(dst)) {
      kk_standard_2d_copy_compressed(cmd, info);
      return;
   }
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_standard_3d_meta_save save;
   kk_standard_3d_meta_begin(cmd, &save, "meta:sparse volume copy");
   const struct vk_meta_copy_image_properties sp = kk_standard_3d_copy_properties(src);
   const struct vk_meta_copy_image_properties dp = kk_standard_3d_copy_properties(dst);
   vk_meta_copy_image(&cmd->vk, &dev->meta, info, &sp, &dp, VK_PIPELINE_BIND_POINT_COMPUTE);
   kk_standard_3d_meta_end(cmd, &save);
}

#include "kk_standard_sparse_3d_clear.h"

#include "kk_standard_sparse_3d_blit.h"
