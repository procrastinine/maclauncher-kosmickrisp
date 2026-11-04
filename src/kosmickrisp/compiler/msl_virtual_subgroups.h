/* SPDX-License-Identifier: MIT */
#ifndef MSL_VIRTUAL_SUBGROUPS_H
#define MSL_VIRTUAL_SUBGROUPS_H
#include "compiler/nir/nir_builder.h"

/* Physical subgroup identity and all original control flow remain native.
 * Only virtual helper participation differs from ordinary shader execution.
 * The caller supplies the current helper state, including native demotion. */
static inline bool
msl_lower_virtual_subgroup(nir_builder *b, nir_intrinsic_instr *intr,
                           nir_def *helper)
{
   nir_def *value = NULL;
   nir_def *live = nir_inot(b, helper);
   switch (intr->intrinsic) {
   case nir_intrinsic_ballot:
      nir_src_rewrite(&intr->src[0], nir_iand(b, intr->src[0].ssa,
         live));
      return true;
   case nir_intrinsic_vote_any:
      nir_src_rewrite(&intr->src[0], nir_iand(b, intr->src[0].ssa,
         live));
      return true;
   case nir_intrinsic_vote_all:
      nir_src_rewrite(&intr->src[0], nir_ior(b, intr->src[0].ssa,
         helper));
      return true;
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan: {
      nir_const_value identity = nir_alu_binop_identity(
         nir_intrinsic_reduction_op(intr), intr->src[0].ssa->bit_size);
      nir_def *neutral = nir_build_imm(b, 1,
         intr->src[0].ssa->bit_size, &identity);
      nir_src_rewrite(&intr->src[0], nir_bcsel(b,
         live, intr->src[0].ssa, neutral));
      return true;
   }
   case nir_intrinsic_elect:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq: {
      /* Match the backend's native 32-lane subgroup ballot. */
      nir_def *mask = nir_ballot(b, 1, 32, live);
      nir_def *first = nir_find_lsb(b, mask);
      if (intr->intrinsic == nir_intrinsic_elect) {
         value = nir_iand(b, live,
            nir_ieq(b, nir_load_subgroup_invocation(b), first));
      } else {
         nir_def *leader = nir_read_invocation(b, intr->src[0].ssa, first);
         if (intr->intrinsic == nir_intrinsic_read_first_invocation) {
            value = leader;
         } else {
            nir_def *equal = intr->intrinsic == nir_intrinsic_vote_feq
               ? nir_feq(b, intr->src[0].ssa, leader)
               : nir_ieq(b, intr->src[0].ssa, leader);
            value = nir_vote_all(b, 1,
               nir_ior(b, equal, nir_inot(b, live)));
         }
      }
      break;
   }
   default: return false;
   }
   nir_def_replace(&intr->def, value);
   return true;
}
#endif
