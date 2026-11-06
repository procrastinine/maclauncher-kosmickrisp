/* SPDX-License-Identifier: MIT */
#ifndef KK_QUAD_CONTROL_H
#define KK_QUAD_CONTROL_H

/* The subgroup pass lowers quad votes to ballots. Keep their quad requirement
 * through later info gathering so fragment triangle merging remains disabled.
 * This filter shares the existing pass's walk and accepts every instruction.
 */
static bool
kk_quad_control_filter(const nir_intrinsic_instr *intr, const void *data)
{
   if (intr->intrinsic == nir_intrinsic_quad_vote_any ||
       intr->intrinsic == nir_intrinsic_quad_vote_all) {
      nir_shader *shader = (nir_shader *)data;
      shader->info.fs.require_full_quads = true;
   }
   return true;
}
#endif
