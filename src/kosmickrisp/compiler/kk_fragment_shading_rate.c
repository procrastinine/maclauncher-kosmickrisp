/* SPDX-License-Identifier: MIT */
#include "kk_fragment_shading_rate.h"
#include "nir_builder.h"
#include <vulkan/vulkan_core.h>

nir_def *
kk_nir_vrs_combine_extent(nir_builder *b, nir_def *a, nir_def *c, nir_def *op)
{
   nir_def *v = nir_imul(b, a, c);
   v = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR), nir_umax(b, a, c), v);
   v = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR), nir_umin(b, a, c), v);
   v = nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR), c, v);
   return nir_bcsel(b, nir_ieq_imm(b, op, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR), a, v);
}

nir_def *
kk_nir_vrs_decode_extent(nir_builder *b, nir_def *rate)
{
   return nir_vec2(b,
      nir_ishl(b, nir_imm_int(b, 1), nir_iand_imm(b, nir_ushr_imm(b, rate, 2), 3)),
      nir_ishl(b, nir_imm_int(b, 1), nir_iand_imm(b, rate, 3)));
}

static bool
lower_fine_fragment_rate(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_frag_shading_rate) return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_imm_int(b, 0));
   return true;
}

void
kk_nir_vrs_lower_fine_rate(nir_shader *nir)
{
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_SHADING_RATE))
      nir_shader_intrinsics_pass(nir, lower_fine_fragment_rate,
                                 nir_metadata_control_flow, NULL);
}
