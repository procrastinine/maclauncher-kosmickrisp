/* SPDX-License-Identifier: MIT */
#ifndef KK_RENDER_SCISSORS_H
#define KK_RENDER_SCISSORS_H
#include "vk_graphics_state.h"
#include "kosmickrisp/bridge/mtl_types.h"
#include "util/u_math.h"

/* rects is zero initialized, including the required discard/empty slot. */
static inline void
kk_render_scissors(const struct vk_viewport_state *vp, const VkRect2D *area,
                   struct mtl_scissor_rect *rects)
{
   VkOffset2D origin = area->offset;
   VkOffset2D end = {.x = origin.x + area->extent.width,
                     .y = origin.y + area->extent.height};
   for (uint32_t i = 0; i < vp->scissor_count; i++) {
      const VkRect2D *rect = &vp->scissors[i];

      size_t x0 = CLAMP(rect->offset.x, origin.x, end.x);
      size_t x1 = CLAMP(rect->offset.x + rect->extent.width, origin.x, end.x);
      size_t y0 = CLAMP(rect->offset.y, origin.y, end.y);
      size_t y1 = CLAMP(rect->offset.y + rect->extent.height, origin.y, end.y);
      size_t minx = MIN2(x0, x1);
      size_t miny = MIN2(y0, y1);
      size_t maxx = MAX2(x0, x1);
      size_t maxy = MAX2(y0, y1);
      rects[i].x = minx;
      rects[i].y = miny;
      rects[i].width = maxx - minx;
      rects[i].height = maxy - miny;
   }

}
#endif
