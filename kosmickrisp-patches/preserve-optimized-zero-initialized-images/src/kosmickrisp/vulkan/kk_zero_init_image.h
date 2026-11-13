/* SPDX-License-Identifier: MIT */
#ifndef KK_ZERO_INIT_IMAGE_H
#define KK_ZERO_INIT_IMAGE_H

/* Included by kk_cmd_meta.c for its ordinary state preservation. ZERO_INITIALIZED
 * is an initial, whole-image layout. It cannot be entered again: no per-image
 * dirty state, draw test or shader selection is needed.
 */
static bool
kk_zero_image_upload(struct kk_cmd_buffer *cmd)
{
   if (cmd->zero_image_upload != VK_NULL_HANDLE)
      return true;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = KK_ZERO_IMAGE_UPLOAD_SIZE,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
   };
   VkBuffer buffer;
   VkResult result = vk_meta_create_buffer(&cmd->vk, &dev->meta, &info, &buffer);
   void *map = NULL;
   if (result == VK_SUCCESS)
      result = dev->meta.cmd_bind_map_buffer(&cmd->vk, &dev->meta, buffer, &map);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return false;
   }
   memset(map, 0, KK_ZERO_IMAGE_UPLOAD_SIZE);
   cmd->zero_image_upload = buffer;
   return true;
}

static void
kk_zero_image_copy(struct kk_cmd_buffer *cmd, struct kk_image *image)
{
   if (!kk_zero_image_upload(cmd))
      return;
   for (uint32_t plane = 0; plane < image->plane_count; ++plane) {
      const VkFormat format = vk_format_get_plane_format(image->vk.format, plane);
      const struct util_format_description *desc = vk_format_description(format);
      const VkImageAspectFlags aspect = image->plane_count > 1
         ? VK_IMAGE_ASPECT_PLANE_0_BIT << plane : VK_IMAGE_ASPECT_COLOR_BIT;
      for (uint32_t level = 0; level < image->vk.mip_levels; ++level) {
         VkExtent3D extent = vk_image_mip_level_extent(&image->vk, level);
         extent.width = vk_format_get_plane_width(image->vk.format, plane, extent.width);
         extent.height = vk_format_get_plane_height(image->vk.format, plane, extent.height);
         for (uint32_t layer = 0; layer < image->vk.array_layers; ++layer) {
            for (uint32_t z = 0; z < extent.depth; ++z) {
               for (uint32_t x = 0; x < extent.width;) {
                  VkExtent2D column = kk_zero_init_tile(extent.width - x, extent.height,
                     desc->block.width, desc->block.height, desc->block.bits / 8);
                  for (uint32_t y = 0; y < extent.height;) {
                     VkExtent2D tile = kk_zero_init_tile(column.width, extent.height - y,
                        desc->block.width, desc->block.height, desc->block.bits / 8);
                     const VkBufferImageCopy2 region = {
                        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                        .imageSubresource = {aspect, level, layer, 1},
                        .imageOffset = {x, y, z},
                        .imageExtent = {tile.width, tile.height, 1},
                     };
                     const VkCopyBufferToImageInfo2 copy = {
                        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                        .srcBuffer = cmd->zero_image_upload,
                        .dstImage = kk_image_to_handle(image),
                        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .regionCount = 1, .pRegions = &region,
                     };
                     kk_CmdCopyBufferToImage2(kk_cmd_buffer_to_handle(cmd), &copy);
                     if (vk_command_buffer_has_error(&cmd->vk))
                        return;
                     y += tile.height;
                  }
                  x += column.width;
               }
            }
         }
      }
   }
}

void
kk_zero_init_image(struct kk_cmd_buffer *cmd, struct kk_image *image)
{
   bool optimized = false;
   for (uint32_t p = 0; p < image->plane_count; ++p)
      optimized |= image->planes[p].layout.optimized_layout;
   if (!optimized || vk_command_buffer_has_error(&cmd->vk))
      return;

   /* Complete preceding memory fills before native image initialization, and
    * leave an ordered boundary for consumers of the newly encoded zero texels.
    * Existing encoder boundaries supply queue/encoder alias visibility.
    */
   cs_end(cmd);
   const struct kk_rendering_state render = cmd->state.gfx.render;
   mtl_render_pass_descriptor *descriptor = cmd->state.gfx.render_pass_descriptor;
   const bool need_start = cmd->state.gfx.need_to_start_render_pass;
   const VkShaderStageFlags stages = cmd->vk.pipeline_shader_stages;
   /* Implicit initial transitions can run while the common render-pass state
    * already names the application's first subpass. Internal clears must not
    * inherit its input-attachment or location mappings.
    */
   struct vk_render_pass *render_pass = cmd->vk.render_pass;
   cmd->vk.render_pass = NULL;
   struct kk_meta_save graphics, compute;
   kk_meta_begin(cmd, &graphics, VK_PIPELINE_BIND_POINT_GRAPHICS, "meta:zero image");
   kk_meta_begin(cmd, &compute, VK_PIPELINE_BIND_POINT_COMPUTE, "meta:zero data");

   const bool depth_stencil = image->vk.aspects &
      (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
   const enum pipe_format format = vk_format_to_pipe_format(image->vk.format);
   const struct kk_va_format *native = kk_get_va_format(format);
   const bool color = native && native->color;
   const bool raw64 = image->vk.format == VK_FORMAT_R64_UINT ||
                      image->vk.format == VK_FORMAT_R64_SINT;
   if (image->plane_count == 1 && (depth_stencil || color || raw64) &&
       (image->planes[0].layout.usage & MTL_TEXTURE_USAGE_RENDER_TARGET)) {
      const VkImageSubresourceRange range = {
         .aspectMask = image->vk.aspects,
         .levelCount = VK_REMAINING_MIP_LEVELS,
         .layerCount = VK_REMAINING_ARRAY_LAYERS,
      };
      if (depth_stencil) {
         const VkClearDepthStencilValue zero = {0};
         kk_CmdClearDepthStencilImage(kk_cmd_buffer_to_handle(cmd),
            kk_image_to_handle(image), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
      } else {
         const VkClearColorValue zero = {0};
         kk_CmdClearColorImage(kk_cmd_buffer_to_handle(cmd),
            kk_image_to_handle(image), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
      }
   } else {
      assert(image->vk.samples == VK_SAMPLE_COUNT_1_BIT);
      kk_zero_image_copy(cmd, image);
   }

   cs_end(cmd);
   kk_meta_end(cmd, &compute, VK_PIPELINE_BIND_POINT_COMPUTE);
   kk_meta_end(cmd, &graphics, VK_PIPELINE_BIND_POINT_GRAPHICS);
   cmd->state.gfx.render = render;
   cmd->state.gfx.render_pass_descriptor = descriptor;
   cmd->state.gfx.need_to_start_render_pass = need_start;
   cmd->vk.pipeline_shader_stages = stages;
   cmd->vk.render_pass = render_pass;
}

#endif
