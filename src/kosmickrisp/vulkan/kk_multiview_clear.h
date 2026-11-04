/* SPDX-License-Identifier: MIT */
#ifndef KK_MULTIVIEW_CLEAR_H
#define KK_MULTIVIEW_CLEAR_H
#include "vulkan/vulkan_core.h"
/* Explicit Layer rectangles need a single-view meta pipeline. A multiview
 * pipeline key would also request native vertex amplification of each rect. */
static inline uint32_t
kk_multiview_clear_rects(uint32_t mask, uint32_t count,
   const VkClearRect *source, VkClearRect *rectangles)
{
   uint32_t n = 0;
   for (uint32_t view = 0; view < 32; ++view) {
      if (!(mask & (1u << view))) continue;
      for (uint32_t r = 0; r < count; ++r) {
         rectangles[n] = source[r];
         rectangles[n].baseArrayLayer = view;
         rectangles[n++].layerCount = 1;
      }
   }
   return n;
}
#endif
