/* SPDX-License-Identifier: MIT */
#ifndef KK_WRITABLE_MSAA_VIEW_IMPL_H
#define KK_WRITABLE_MSAA_VIEW_IMPL_H

/* Included once by kk_image_view.c. Table and native views are owned by the
 * actual view for its complete Vulkan lifetime, including copied descriptors.
 */
VkResult
kk_writable_msaa_view_init(struct kk_device *dev, struct kk_image_view *view)
{
   struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   if (!image->writable_msaa ||
       !(view->vk.usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)))
      return VK_SUCCESS;
   struct kk_writable_msaa_view *storage = calloc(1, sizeof(*storage));
   if (!storage)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   view->writable_msaa = storage;
   storage->sample_count = image->vk.samples;
   assert(view->plane_count == 1);
   assert(view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
   const uint64_t table_size_B = kk_writable_msaa_table_size_B(image->vk.samples);
   VkResult result = kk_alloc_bo(dev, &view->vk.base,
      table_size_B, 16, &storage->table);
   if (result != VK_SUCCESS)
      goto fail;
   if (storage->table->size_B < table_size_B || !storage->table->gpu ||
       storage->table->gpu >
       UINT64_MAX - table_size_B) {
      result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto fail;
   }
   const enum pipe_format format = vk_format_to_pipe_format(view->vk.format);
   const struct kk_va_format *caps = kk_get_va_format(format);
   struct kk_view_layout layout = {
      .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .sample_count_sa = VK_SAMPLE_COUNT_1_BIT,
      .format = {.pipe = kk_format_backing(format), .mtl = caps->mtl_pixel_format},
      .base_level = 0,
      .num_levels = 1,
      .base_array_layer = view->vk.base_array_layer,
      .array_len = view->vk.layer_count,
   };
   const uint8_t swizzle[4] = {
      vk_swizzle_to_pipe(view->vk.swizzle.r),
      vk_swizzle_to_pipe(view->vk.swizzle.g),
      vk_swizzle_to_pipe(view->vk.swizzle.b),
      vk_swizzle_to_pipe(view->vk.swizzle.a),
   };
   util_format_compose_swizzles(caps->unswizzle.channels,
                                  swizzle, layout.swizzle.channels);
   uint64_t *ids = storage->table->cpu;
   assert(ids);
   for (uint32_t s = 0; s < image->vk.samples; s++) {
      storage->sample[s] = mtl_new_texture_view_with(
         image->writable_msaa->sample[s], &layout);
      if (!storage->sample[s]) {
         result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto fail;
      }
      ids[s] = mtl_texture_get_gpu_resource_id(storage->sample[s]);
   }
   /* Defaults describe the logical view, before any backing-format unswizzle. */
   struct kk_writable_msaa_defaults *defaults = (void *)
      ((uint8_t *)ids + kk_writable_msaa_defaults_offset_B(image->vk.samples));
   defaults->storage_alpha_one = !vk_format_has_alpha(view->vk.format);
   defaults->sampled_default_one_mask = 0;
   for (unsigned c = 0; c < 4; c++) {
      if (swizzle[c] == PIPE_SWIZZLE_1 ||
          (swizzle[c] == PIPE_SWIZZLE_W && defaults->storage_alpha_one))
         defaults->sampled_default_one_mask |= 1u << c;
   }
   return VK_SUCCESS;
fail:
   kk_writable_msaa_view_finish(dev, view);
   return result;
}

void
kk_writable_msaa_view_finish(struct kk_device *dev, struct kk_image_view *view)
{
   struct kk_writable_msaa_view *storage = view->writable_msaa;
   if (!storage)
      return;
   /* Meta views may retire when their command buffer resets, after the
    * application has legally destroyed the image following GPU completion.
    * Their retained textures and count must be independent of that image.
    */
   for (uint32_t s = 0; s < storage->sample_count; s++)
      if (storage->sample[s])
         mtl_release(storage->sample[s]);
   if (storage->table)
      kk_destroy_bo(dev, storage->table);
   free(storage);
   view->writable_msaa = NULL;
}

void
kk_writable_msaa_storage_descriptor(struct kk_storage_image_descriptor *desc,
                                     const struct kk_image_view *view)
{
   if (!view->writable_msaa)
      return;
   desc->image_gpu_resource_id = view->writable_msaa->table->gpu;
   desc->texel_offset = view->vk.image->samples;
   desc->texel_count = view->vk.layer_count;
}

void
kk_writable_msaa_sampled_descriptor(struct kk_sampled_image_descriptor *desc,
   const struct kk_image_view *view, bool input_attachment)
{
   if (!view->writable_msaa || input_attachment)
      return;
   desc->aux_resource_id = view->writable_msaa->table->gpu;
   desc->aux_word = ((uint64_t)view->vk.layer_count << 32) |
                   (uint32_t)view->vk.image->samples;
}
#endif
