/* SPDX-License-Identifier: MIT */
#ifndef KK_NATIVE_BLOCK_ALIAS_H
#define KK_NATIVE_BLOCK_ALIAS_H
#include "kk_image_layout.h"
#include "kosmickrisp/bridge/mtl_types.h"
struct kk_device;
struct kk_image_plane;
struct kk_view_layout;

/* Native handles only. The caller owns physical image/page lifetime,
 * coordinates, publication, coherence and deferred GPU retirement. */
struct kk_layered_bc_layer {
   mtl_texture *base, *sampled, *storage;
};
VkResult kk_layered_bc_native_alias_init(struct kk_device *dev,
   struct kk_image_plane *plane, const struct kk_image_layout *physical_layout,
   uint64_t physical_offset_B, mtl_texture *canonical,
   const struct kk_view_layout *sampled_layout,
   const struct kk_view_layout *storage_layout,
   struct kk_layered_bc_layer *layer);
void kk_layered_bc_native_alias_finish(struct kk_layered_bc_layer *layer);
#endif
