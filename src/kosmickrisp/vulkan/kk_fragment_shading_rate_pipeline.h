/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_PIPELINE_H
#define KK_FRAGMENT_SHADING_RATE_PIPELINE_H
#include "kosmickrisp/compiler/kk_fragment_shading_rate.h"

static inline bool
kk_vrs_combiner_is_fine(bool a, bool b, VkFragmentShadingRateCombinerOpKHR op)
{
   switch (op) {
   case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR:
      return a;
   case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR:
      return b;
   case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR:
      return a || b;
   case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR:
   case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MUL_KHR:
      return a && b;
   default:
      return false;
   }
}

static inline bool
kk_vrs_pipeline_can_coarsen(const struct vk_graphics_pipeline_state *state,
                          bool primitive_rate, bool attachment_rate)
{
   /* Incomplete library state and dynamic combiners may select later rates.
    * An unavailable input contributes 1x1; MIN with 1x1 always stays fine. */
   if (!state || BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_FSR) || !state->fsr)
      return true;
   const struct vk_fragment_shading_rate_state *fsr = state->fsr;
   bool fine = fsr->fragment_size.width == 1 && fsr->fragment_size.height == 1;
   fine = kk_vrs_combiner_is_fine(fine, !primitive_rate, fsr->combiner_ops[0]);
   fine = kk_vrs_combiner_is_fine(fine, !attachment_rate, fsr->combiner_ops[1]);
   return !fine;
}

/* Borrowed descriptor: caller must hold the owning shader through completion.
 * This does not select or advertise any Poly, mesh or GPL combination. */
mtl_render_pipeline_descriptor *
kk_vrs_native_pipeline_descriptor(const struct kk_shader *shader)
{
   return shader && shader->attachment_pipelines
      ? shader->attachment_pipelines->descriptor[0] : NULL;
}
#endif
