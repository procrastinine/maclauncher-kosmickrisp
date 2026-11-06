/* SPDX-License-Identifier: MIT */
#ifndef KOSMICKRISP_FP32_PRESERVE_H
#define KOSMICKRISP_FP32_PRESERVE_H
#include "kk_fp32_preserve_msl.h"
static bool kk_fp32_preserve(const nir_shader *shader)
{
   return nir_is_denorm_preserve(shader->info.float_controls_execution_mode,
                                 32);
}
static bool kk_fp32_preserve_alu(struct nir_to_msl_ctx *ctx, nir_alu_instr *alu)
{
   if (!kk_fp32_preserve(ctx->shader))
      return false;
   const char *helper = NULL;
   if (alu->src[0].src.ssa->bit_size != 32)
      return false;
   switch (alu->op) {
   case nir_op_fadd:
      helper = "kk_dp_add";
      break;
   case nir_op_fsub:
      helper = "kk_dp_subtract";
      break;
   case nir_op_fmul:
      helper = "kk_dp_mul";
      break;
   case nir_op_ffma:
      helper = "kk_dp_fma";
      break;
   case nir_op_flrp:
      helper = "kk_dp_lrp";
      break;
   case nir_op_fdiv:
      helper = "kk_dp_div";
      break;
   case nir_op_frcp:
      P(ctx, "kk_dp_div(1.0f, ");
      alu_src_to_msl(ctx, alu, 0);
      P(ctx, ")");
      return true;
   case nir_op_fsqrt:
      helper = "kk_dp_sqrt";
      break;
   case nir_op_frsq:
      helper = "kk_dp_rsqrt";
      break;
   case nir_op_fmin:
      helper = "kk_dp_min";
      break;
   case nir_op_fmax:
      helper = "kk_dp_max";
      break;
   case nir_op_fsat:
      helper = "kk_dp_sat";
      break;
   case nir_op_feq:
      helper = "kk_dp_eq";
      break;
   case nir_op_fneu:
      helper = "kk_dp_ne";
      break;
   case nir_op_flt:
      helper = "kk_dp_lt";
      break;
   case nir_op_fge:
      helper = "kk_dp_ge";
      break;
   case nir_op_ldexp:
      helper = "kk_dp_ldexp";
      break;
   case nir_op_ffloor:
      helper = "kk_dp_floor";
      break;
   case nir_op_fceil:
      helper = "kk_dp_ceil";
      break;
   case nir_op_fsign:
      helper = "kk_dp_sign";
      break;
   case nir_op_fsin:
      helper = "kk_dp_sin";
      break;
   case nir_op_flog2:
      helper = "kk_dp_log2";
      break;
   case nir_op_fexp2:
      helper = "kk_dp_exp2";
      break;
   case nir_op_fpow:
      helper = "kk_dp_pow";
      break;
   case nir_op_ffract:
      helper = "kk_dp_fract";
      break;
   case nir_op_frem:
      helper = "kk_dp_fmod";
      break;
   default:
      return false;
   }
   alu_funclike(ctx, alu, helper);
   return true;
}
/* Generic nir_lower_frexp assumes normalized significands. Normalize FP32
 * subnormals with integer bits before that pass; FP16 follows its native path.
 */
static bool kk_fp32_preserve_prepare(nir_builder *b, nir_instr *instr,
                                   UNUSED void *data)
{
   if (instr->type != nir_instr_type_alu || !kk_fp32_preserve(b->shader))
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   /* Choose GLSL Fma's permitted fused implementation before NIR can split
    * the weak form. Its complete product must reach the single RTE pack. */
   if (alu->op == nir_op_ffma_weak && alu->def.bit_size == 32) {
      alu->op = nir_op_ffma;
      alu->fp_math_ctrl |= nir_fp_exact;
      return true;
   }
   if ((alu->op != nir_op_frexp_sig && alu->op != nir_op_frexp_exp) ||
       alu->src[0].src.ssa->bit_size != 32)
      return false;
   b->cursor = nir_before_instr(instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   nir_def *mag = nir_iand_imm(b, x, 0x7fffffff);
   nir_def *raw_exp = nir_ushr_imm(b, mag, 23);
   nir_def *sub = nir_ieq_imm(b, raw_exp, 0);
   nir_def *shift = nir_isub(b, nir_imm_int(b, 23), nir_ufind_msb(b, mag));
   shift = nir_bcsel(b, sub, shift, nir_imm_int(b, 0));
   nir_def *mant = nir_bcsel(b, sub, nir_ishl(b, mag, shift), mag);
   nir_def *finite_nonzero =
       nir_iand(b, nir_ine_imm(b, mag, 0), nir_ult_imm(b, mag, 0x7f800000));
   nir_def *value;
   if (alu->op == nir_op_frexp_sig) {
      value =
          nir_ior(b, nir_iand_imm(b, x, 0x80000000),
                  nir_ior_imm(b, nir_iand_imm(b, mant, 0x7fffff), 0x3f000000));
      value = nir_bcsel(b, finite_nonzero, value, x);
   } else {
      value = nir_bcsel(b, sub, nir_isub(b, nir_imm_int(b, -125), shift),
                        nir_iadd_imm(b, raw_exp, -126));
      value = nir_bcsel(b, finite_nonzero, value, nir_imm_int(b, 0));
   }
   nir_def_rewrite_uses(&alu->def, value);
   nir_instr_remove(instr);
   return true;
}
#endif
