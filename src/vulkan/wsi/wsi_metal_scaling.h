/* SPDX-License-Identifier: MIT */
#ifndef KK_WSI_SCALING_H
#define KK_WSI_SCALING_H

/* Integer pixel rectangles for the existing Metal presentation blit.  The
 * one-to-one case clips both rectangles together, preserving pixel identity. */
static inline int32_t
wsi_metal_gravity_offset(int32_t space, VkPresentGravityFlagsKHR gravity)
{
   if (gravity == VK_PRESENT_GRAVITY_MIN_BIT_KHR)
      return 0;
   if (gravity == VK_PRESENT_GRAVITY_MAX_BIT_KHR)
      return space;
   return space / 2;
}

static inline VkImageBlit2
wsi_metal_present_region(VkExtent2D source, VkExtent2D target,
                         VkPresentScalingFlagsKHR scaling,
                         VkPresentGravityFlagsKHR gravity_x,
                         VkPresentGravityFlagsKHR gravity_y)
{
   uint32_t width = source.width, height = source.height;
   if (scaling == VK_PRESENT_SCALING_STRETCH_BIT_KHR) {
      width = target.width;
      height = target.height;
   } else if (scaling == VK_PRESENT_SCALING_ASPECT_RATIO_STRETCH_BIT_KHR) {
      if ((uint64_t)target.width * source.height <=
          (uint64_t)target.height * source.width) {
         width = target.width;
         height = MAX2(1, (uint64_t)source.height * width / source.width);
      } else {
         height = target.height;
         width = MAX2(1, (uint64_t)source.width * height / source.height);
      }
   }
   int32_t x = wsi_metal_gravity_offset((int32_t)target.width - width, gravity_x);
   int32_t y = wsi_metal_gravity_offset((int32_t)target.height - height, gravity_y);
   VkImageBlit2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffsets = {{0, 0, 0}, {source.width, source.height, 1}},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffsets = {{x, y, 0}, {x + width, y + height, 1}},
   };
   if (scaling == VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR) {
      region.srcOffsets[0].x = MAX2(-x, 0);
      region.srcOffsets[0].y = MAX2(-y, 0);
      region.dstOffsets[0].x = MAX2(x, 0);
      region.dstOffsets[0].y = MAX2(y, 0);
      region.dstOffsets[1].x = MIN2(x + width, target.width);
      region.dstOffsets[1].y = MIN2(y + height, target.height);
      region.srcOffsets[1].x = region.srcOffsets[0].x +
         region.dstOffsets[1].x - region.dstOffsets[0].x;
      region.srcOffsets[1].y = region.srcOffsets[0].y +
         region.dstOffsets[1].y - region.dstOffsets[0].y;
   }
   return region;
}
#endif
