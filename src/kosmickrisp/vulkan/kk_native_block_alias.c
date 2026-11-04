/* SPDX-License-Identifier: MIT */
#include "kk_native_block_alias.h"
#include "kk_device.h"
#include "kk_image.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

void
kk_layered_bc_native_alias_finish(struct kk_layered_bc_layer *layer)
{
   if (layer->sampled) mtl_release(layer->sampled);
   if (layer->storage) mtl_release(layer->storage);
   if (layer->base) mtl_release(layer->base);
   *layer = (struct kk_layered_bc_layer){0};
}

VkResult
kk_layered_bc_native_alias_init(struct kk_device *dev,
   struct kk_image_plane *plane, const struct kk_image_layout *physical_layout,
   uint64_t physical_offset_B, mtl_texture *canonical,
   const struct kk_view_layout *sampled_layout,
   const struct kk_view_layout *storage_layout,
   struct kk_layered_bc_layer *layer)
{
   assert(!layer->base && !layer->sampled && !layer->storage);
   assert(canonical || (plane && physical_layout));
   if (canonical) {
      layer->base = mtl_retain(canonical);
   } else {
      struct kk_image_layout layout = *physical_layout;
      layer->base = kk_image_plane_create_texture(plane, &layout,
                                                   physical_offset_B);
   }
   if (!layer->base)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   if (sampled_layout)
      layer->sampled = mtl_new_texture_view_with(layer->base, sampled_layout);
   if (storage_layout)
      layer->storage = mtl_new_texture_view_with(layer->base, storage_layout);
   if ((sampled_layout && !layer->sampled) ||
       (storage_layout && !layer->storage)) {
      kk_layered_bc_native_alias_finish(layer);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   return VK_SUCCESS;
}

