/* SPDX-License-Identifier: MIT */
#ifndef KK_WRITABLE_MSAA_IMAGE_IMPL_H
#define KK_WRITABLE_MSAA_IMAGE_IMPL_H

/* This implementation is included once by kk_image.c. It uses ordinary image,
 * heap and residency helpers. The native image is an internal logical alias of
 * the original native resource, not a separately allocated cache or heap.
 */
VkResult
kk_writable_msaa_image_init(struct kk_device *dev, struct kk_image *image,
                            VkImageLayout initial_layout)
{
   if (!kk_writable_msaa_requested(&image->vk))
      return VK_SUCCESS;
   if (image->vk.samples > KK_WRITABLE_MSAA_MAX_SAMPLES ||
       image->vk.image_type != VK_IMAGE_TYPE_2D || image->plane_count != 1 ||
       image->vk.aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
       image->vk.tiling != VK_IMAGE_TILING_OPTIMAL ||
       (image->vk.create_flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
         VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
         VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)))
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);

   struct kk_writable_msaa_image *storage = calloc(1, sizeof(*storage));
   if (!storage)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   image->writable_msaa = storage;
   struct vk_image physical = image->vk;
   physical.samples = VK_SAMPLE_COUNT_1_BIT;
   physical.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   physical.create_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   enum pipe_format format = vk_format_to_pipe_format(image->vk.format);
   kk_image_layout_init(dev, &physical, format, 1, 1,
                         &storage->sample_layout, initial_layout, true);
   /* Each sample retains every logical layer. No dimensions or global array
    * limits are divided by the sample count.
    */
   storage->sample_stride_B = align64(storage->sample_layout.size_B,
                                      storage->sample_layout.align_B);
   storage->sample_offset_B = align64(image->planes[0].layout.size_B,
                                      storage->sample_layout.align_B);
   if (storage->sample_stride_B >
       (UINT64_MAX - storage->sample_offset_B) / image->vk.samples) {
      free(storage);
      image->writable_msaa = NULL;
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   storage->total_size_B = storage->sample_offset_B +
      storage->sample_stride_B * image->vk.samples;
   storage->align_B = MAX2(image->planes[0].layout.align_B,
                            storage->sample_layout.align_B);

   const VkImageCreateInfo native = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = vk_format_from_pipe_format(kk_writable_msaa_raw_format(format)),
      .extent = image->vk.extent,
      .mipLevels = 1,
      .arrayLayers = image->vk.array_layers,
      .samples = image->vk.samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult result = kk_CreateImage(kk_device_to_handle(dev), &native, NULL,
                                    &storage->native_image);
   if (result != VK_SUCCESS) {
      free(storage);
      image->writable_msaa = NULL;
   }
   return result;
}

VkResult
kk_writable_msaa_image_bind(struct kk_device *dev, struct kk_image *image)
{
   struct kk_writable_msaa_image *storage = image->writable_msaa;
   if (!storage)
      return VK_SUCCESS;
   struct kk_image_plane *plane = &image->planes[0];
   for (uint32_t s = 0; s < image->vk.samples; s++) {
      storage->sample[s] = kk_image_plane_create_texture(plane,
         &storage->sample_layout,
         storage->sample_offset_B + s * storage->sample_stride_B);
      if (!storage->sample[s])
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   VK_FROM_HANDLE(kk_image, native, storage->native_image);
   struct kk_image_plane *native_plane = &native->planes[0];
   native_plane->mem = plane->mem;
   native_plane->mem_offset_B = plane->mem_offset_B;
   native_plane->addr = plane->addr;
   const enum pipe_format raw = kk_writable_msaa_raw_format(
      vk_format_to_pipe_format(image->vk.format));
   struct kk_view_layout raw_view = {
      .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .sample_count_sa = image->vk.samples,
      .format = {.pipe = raw, .mtl = kk_get_va_format(raw)->mtl_pixel_format},
      .num_levels = 1,
      .array_len = image->vk.array_layers,
   };
   native_plane->mtl_handle = mtl_new_texture_view_with_no_swizzle(
      plane->mtl_handle, &raw_view);
   if (!native_plane->mtl_handle)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   return VK_SUCCESS;
}

void
kk_writable_msaa_image_finish(struct kk_device *dev, struct kk_image *image)
{
   struct kk_writable_msaa_image *storage = image->writable_msaa;
   if (!storage)
      return;
   for (uint32_t s = 0; s < image->vk.samples; s++)
      if (storage->sample[s])
         mtl_release(storage->sample[s]);
   if (storage->native_image != VK_NULL_HANDLE)
      kk_DestroyImage(kk_device_to_handle(dev), storage->native_image, NULL);
   free(storage);
   image->writable_msaa = NULL;
}
#endif
