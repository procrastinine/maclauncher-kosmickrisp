/* SPDX-License-Identifier: MIT */

#ifndef KK_MIXED_FLOAT_DOT_H
#define KK_MIXED_FLOAT_DOT_H

#include "nir_builder.h"

/* The mixed operation converts both half vectors to float before accumulating.
 * Lower before float-control legalization so explicit preservation modes apply
 * to the same native conversions and FMAs as ordinary float arithmetic.
 */
static bool
kk_lower_mixed_float_dot(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_f16dot2_fadd || alu->def.bit_size != 32)
      return false;

   b->cursor = nir_before_instr(instr);
   uint32_t saved_fp_math_ctrl = b->fp_math_ctrl;
   /* These widenings observe actual half values. The inexact algebraic rule
    * f2f32(f2f16(x)) -> x must not erase the input rounding boundary merely
    * because the mixed dot's accumulation permits arithmetic transforms.
    */
   b->fp_math_ctrl = alu->fp_math_ctrl | nir_fp_exact;
   nir_def *a = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 0));
   nir_def *c = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 1));
   b->fp_math_ctrl = alu->fp_math_ctrl;
   nir_def *acc = nir_ssa_for_alu_src(b, alu, 2);
   for (unsigned i = 0; i < 2; i++)
      acc = nir_ffma(b, nir_channel(b, a, i), nir_channel(b, c, i), acc);
   b->fp_math_ctrl = saved_fp_math_ctrl;
   nir_def_replace(&alu->def, acc);
   return true;
}

#endif
