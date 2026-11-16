// SPDX-License-Identifier: MIT
// Adapted from steamac patch 0071 (https://github.com/fxgl/steamac).
// Original author: steamac. Copyright 2026 FX GAMES FZ LLC and patch authors.
// steamac is Apache-2.0; its changes to Mesa follow Mesa MIT, per NOTICE.
// See steamac-NOTICE.txt, steamac-LICENSE.txt and steamac-MIT.txt.
// Stop compressed-copy extents at the destination mip edge
export const id = "stop-compressed-copy-extents-at-the-destination";
export const sourceFiles = ['kosmickrisp-patches/steamac-NOTICE.txt', 'kosmickrisp-patches/steamac-LICENSE.txt', 'kosmickrisp-patches/steamac-MIT.txt'];
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_copy.c", source => source
    .replace("   }\n\n   /* After adjusting for compression, sanitize destination extents for 1D/2D */\n   if (dst_plane->layout.height_px == 1)\n      dst_size.y = 1;\n   if (dst_plane->layout.depth_px == 1)\n      dst_size.z = 1;\n\n   struct mtl_origin src_origin =\n",
      () => "   }\n\n   /* After adjusting for compression, stop the destination extent at the\n    * destination mip level's edge. A block expanded into a compressed mip\n    * tail can cover fewer texels than the block (one RG32 texel copied into a\n    * 1x1 BC1 level), which Vulkan allows and Metal rejects. This also keeps\n    * 1D and 2D destinations at a height or depth of 1. */\n   const VkExtent3D dst_extent =\n      vk_image_mip_level_extent(&dst->vk, region->dstSubresource.mipLevel);\n   dst_size.x = MIN2(dst_size.x, dst_extent.width - region->dstOffset.x);\n   dst_size.y = MIN2(dst_size.y, dst_extent.height - region->dstOffset.y);\n   dst_size.z = MIN2(dst_size.z, dst_extent.depth - region->dstOffset.z);\n\n   struct mtl_origin src_origin =\n"));
}
