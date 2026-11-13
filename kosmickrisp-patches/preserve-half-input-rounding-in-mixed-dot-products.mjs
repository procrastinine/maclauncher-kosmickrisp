// SPDX-License-Identifier: MIT
// Preserve half-input rounding in mixed dot products
export const id = "preserve-half-input-rounding-in-mixed-dot-products";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/kk_mixed_float_dot.h", source => source
    .replace("\n   b->cursor = nir_before_instr(instr);\n   uint32_t saved_fp_math_ctrl = b->fp_math_ctrl;\n   b->fp_math_ctrl = alu->fp_math_ctrl;\n   nir_def *a = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 0));\n   nir_def *c = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 1));\n   nir_def *acc = nir_ssa_for_alu_src(b, alu, 2);\n",
      () => "\n   b->cursor = nir_before_instr(instr);\n   uint32_t saved_fp_math_ctrl = b->fp_math_ctrl;\n   /* These widenings observe actual half values. The inexact algebraic rule\n    * f2f32(f2f16(x)) -> x must not erase the input rounding boundary merely\n    * because the mixed dot's accumulation permits arithmetic transforms.\n    */\n   b->fp_math_ctrl = alu->fp_math_ctrl | nir_fp_exact;\n   nir_def *a = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 0));\n   nir_def *c = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 1));\n   nir_def *acc = nir_ssa_for_alu_src(b, alu, 2);\n")
    .replace("   b->fp_math_ctrl = alu->fp_math_ctrl | nir_fp_exact;\n   nir_def *a = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 0));\n   nir_def *c = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 1));\n   nir_def *acc = nir_ssa_for_alu_src(b, alu, 2);\n   for (unsigned i = 0; i < 2; i++)\n      acc = nir_ffma(b, nir_channel(b, a, i), nir_channel(b, c, i), acc);\n",
      () => "   b->fp_math_ctrl = alu->fp_math_ctrl | nir_fp_exact;\n   nir_def *a = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 0));\n   nir_def *c = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, 1));\n   b->fp_math_ctrl = alu->fp_math_ctrl;\n   nir_def *acc = nir_ssa_for_alu_src(b, alu, 2);\n   for (unsigned i = 0; i < 2; i++)\n      acc = nir_ffma(b, nir_channel(b, a, i), nir_channel(b, c, i), acc);\n"));
}
