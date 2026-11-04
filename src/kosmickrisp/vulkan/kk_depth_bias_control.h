/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BIAS_CONTROL_H
#define KK_DEPTH_BIAS_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "vk_graphics_state.h"

enum kk_depth_bias_control {
   KK_DEPTH_BIAS_CONTROLLED = 1u,
   KK_DEPTH_BIAS_DYNAMIC_ENABLE = 2u,
   KK_DEPTH_BIAS_DYNAMIC_FACTORS = 4u,
};

/* Compute eligibility once for the complete graphics pipeline. Only fields
 * the pipeline declares dynamic need to be read again during a draw. */
static inline uint8_t
kk_depth_bias_control_flags(bool enabled, bool alternate,
   const struct vk_graphics_pipeline_state *state)
{
   /* FORMAT bias is native on the supported D16 and D32 formats, with
    * a unit factor passed to Metal. Only alternate representations need
    * a captured sibling. Devices without those features never build one. */
   if (!enabled || !alternate || !state || !state->rs)
      return 0;
   bool enable = BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE);
   bool factors = BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
   if ((!enable && !state->rs->depth_bias.enable) ||
       (!factors && state->rs->depth_bias.representation ==
           VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT))
      return 0;
   return KK_DEPTH_BIAS_CONTROLLED |
      (enable ? KK_DEPTH_BIAS_DYNAMIC_ENABLE : 0) |
      (factors ? KK_DEPTH_BIAS_DYNAMIC_FACTORS : 0);
}

static inline bool
kk_depth_bias_control_active(uint8_t flags,
   const struct vk_rasterization_state *rs)
{
   return flags &&
      (!(flags & KK_DEPTH_BIAS_DYNAMIC_ENABLE) || rs->depth_bias.enable) &&
      (!(flags & KK_DEPTH_BIAS_DYNAMIC_FACTORS) ||
       rs->depth_bias.representation !=
          VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT);
}

/* Reuse the existing native/Poly alternative only when legal dynamic state
 * can make this controlled pipeline native. Static POINT and conservative
 * overestimate still require their original expansion. */
static inline bool
kk_depth_bias_native_possible(uint8_t flags,
   const struct vk_graphics_pipeline_state *state)
{
   return (flags & (KK_DEPTH_BIAS_DYNAMIC_ENABLE | KK_DEPTH_BIAS_DYNAMIC_FACTORS)) &&
      (state->rs->polygon_mode != VK_POLYGON_MODE_POINT ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE)) &&
      (state->rs->conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_CONSERVATIVE_MODE));
}
#endif
