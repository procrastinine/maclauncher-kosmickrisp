/* SPDX-License-Identifier: MIT */
#ifndef KK_HELPER_SIDE_EFFECTS_H
#define KK_HELPER_SIDE_EFFECTS_H

#include "nir.h"

/* Check actual late NIR: API info may predate polygon/culling lowerings. */
static bool
kk_nir_lower_helper_side_effects(nir_shader *nir)
{
   /* The final SSA gather includes discards inserted by all late lowerings. */
   if (nir->info.stage != MESA_SHADER_FRAGMENT || !nir->info.fs.uses_discard ||
       (!nir->info.writes_memory &&
        !BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_HELPER_INVOCATION)))
      return false;

   bool demotes = false;
   bool terminates = false;
   bool writes = false;
   bool queries = false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            writes |= nir_intrinsic_writes_external_memory(intr);
            queries |= intr->intrinsic == nir_intrinsic_is_helper_invocation;
            switch (intr->intrinsic) {
            case nir_intrinsic_demote:
            case nir_intrinsic_demote_if:
               demotes = true;
               break;
            case nir_intrinsic_terminate:
            case nir_intrinsic_terminate_if:
               terminates = true;
               break;
            default:
               break;
            }
         }
      }
   }
   const bool guard_writes = (demotes || terminates) && writes;
   const bool track_helpers = demotes && (queries || guard_writes);
   if (!guard_writes && !track_helpers)
      return false;

   /* The shared write pass creates is_helper_invocation queries. Lower those
    * afterwards so each query reads initial-helper OR explicit demote state.
    * Give guarded atomic results a defined zero before undef optimizations.
    * Terminate retains the backend's explicit return and is not demoted.
    */
   if (guard_writes) {
      NIR_PASS(_, nir, nir_lower_helper_writes, true);
      NIR_PASS(_, nir, nir_lower_undef_to_zero, NULL);
   }
   if (track_helpers)
      NIR_PASS(_, nir, nir_lower_is_helper_invocation);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_remove_phis);
   return true;
}
#endif
