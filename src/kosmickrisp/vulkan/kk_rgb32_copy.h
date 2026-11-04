/* SPDX-License-Identifier: MIT */
/* RGB32 transfers use bounded RGBA32 staging; only the requested RGB words
 * enter or leave the application's buffer. Image-to-image copies already use
 * the physical layout format, including cross-type bit-preserving copies. */
static bool
kk_prepare_rgb32_copy(struct kk_cmd_buffer *cmd)
{
   /* Preflight once per copy command, before staging or region/chunk loops.
    * Dispatch and preflight share the device-owned per-program slot. */
   VkResult result = kk_device_ensure_precompiled_shader(
      kk_cmd_buffer_device(cmd), LIBKK_RGB32_COPY);
   if (unlikely(result != VK_SUCCESS)) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return false;
   }
   return true;
}

static void
kk_copy_rgb32_buffer_image(struct kk_cmd_buffer *cmd, struct kk_buffer *buffer,
                          struct kk_image *image,
                          const VkBufferImageCopy2 *region, bool to_image)
{
   struct kk_image_plane *plane = &image->planes[0];
   uint32_t width = region->imageExtent.width;
   uint32_t height = region->imageExtent.height;
   bool is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D;
   uint32_t slices = is_3d ? region->imageExtent.depth :
      vk_image_subresource_layer_count(&image->vk, &region->imageSubresource);
   uint64_t row_B = (uint64_t)(region->bufferRowLength ?
      region->bufferRowLength : width) * 12;
   uint64_t slice_B = row_B * (region->bufferImageHeight ?
      region->bufferImageHeight : height);
   uint32_t rows = MIN2(height, MAX2(1u, (4u * 1024u * 1024u) / (width * 16u)));
   struct kk_ptr staging = kk_pool_alloc(cmd, (uint64_t)width * rows * 16u, 16);
   if (unlikely(!staging.gpu))
      return;
   uint32_t alpha = image->vk.format == VK_FORMAT_R32G32B32_SFLOAT ?
      0x3f800000u : 1u;
   mtl_compute_encoder *encoder = cs_get_compute(cmd);

   for (uint32_t z = 0; z < slices; ++z) {
      for (uint32_t y = 0; y < height; y += rows) {
         uint32_t chunk_rows = MIN2(rows, height - y);
         uint64_t packed = buffer->vk.device_address + region->bufferOffset +
                           z * slice_B + y * row_B;
         struct mtl_buffer_image_copy copy = {
            .buffer = staging.buffer,
            .buffer_offset_B = staging.offset,
            .buffer_stride_B = width * 16u,
            .buffer_2d_image_size_B = is_3d ? width * chunk_rows * 16u : 0,
            .image = plane->mtl_handle,
            .image_origin = {
               .x = region->imageOffset.x,
               .y = region->imageOffset.y + y,
               .z = is_3d ? region->imageOffset.z + z : 0,
            },
            .image_size = {.x = width, .y = chunk_rows, .z = 1},
            .image_level = region->imageSubresource.mipLevel,
            .image_slice = is_3d ? 0 : region->imageSubresource.baseArrayLayer + z,
            .options = MTL_BLIT_OPTION_NONE,
         };
         if (to_image) {
            libkk_rgb32_copy(cmd, kk_grid_2d(width, chunk_rows), false,
                             packed, staging.gpu, row_B, width, chunk_rows,
                             true, alpha);
            KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder, MTL_STAGE_DISPATCH,
                                             MTL_STAGE_BLIT, MTL_VISIBILITY_RESOURCE_ALIAS);
            mtl_copy_from_buffer_to_texture(encoder, &copy);
            KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder, MTL_STAGE_BLIT,
                                             MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
         } else {
            mtl_copy_from_texture_to_buffer(encoder, &copy);
            KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder, MTL_STAGE_BLIT,
                                             MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
            libkk_rgb32_copy(cmd, kk_grid_2d(width, chunk_rows), false,
                             packed, staging.gpu, row_B, width, chunk_rows,
                             false, alpha);
            KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder, MTL_STAGE_DISPATCH,
                                             MTL_STAGE_BLIT, MTL_VISIBILITY_RESOURCE_ALIAS);
         }
      }
   }
}
