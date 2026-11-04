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

void
kk_standard_3d_copy_image(struct kk_cmd_buffer *cmd, const VkCopyImageInfo2 *info)
{
   VK_FROM_HANDLE(kk_image, src, info->srcImage);
   VK_FROM_HANDLE(kk_image, dst, info->dstImage);
   assert(kk_image_is_standard_3d(src) || kk_image_is_standard_3d(dst));
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
