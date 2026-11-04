/* SPDX-License-Identifier: MIT */
#ifndef KOSMICKRISP_FP32_PRESERVE_H
#define KOSMICKRISP_FP32_PRESERVE_H
#include "kk_fp32_preserve_msl.h"
#include "kk_fp32_flush_msl.h"
static bool kk_fp32_preserve(const nir_shader *shader)
{
   return nir_is_denorm_preserve(shader->info.float_controls_execution_mode,
                                 32);
}
/* Select an emitter form once, after all optimization. Its ordinary NIR ALU
 * source types, swizzles and math-control flags remain intact. Unaffected
 * shaders enter the original ALU dispatch without a feature probe. */
static bool
kk_fp32_preserve_instr(UNUSED nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->src[0].src.ssa->bit_size != 32)
      return false;

   const char *mode = alu_fp_math_mode_pragma(data, alu);
   switch (alu->op) {
   case nir_op_fadd: alu->op = nir_op_fadd_preserve_kk; break;
   case nir_op_fsub: alu->op = nir_op_fsub_preserve_kk; break;
   case nir_op_fmul: alu->op = nir_op_fmul_preserve_kk; break;
   case nir_op_ffma: alu->op = nir_op_ffma_preserve_kk; break;
   case nir_op_flrp: alu->op = nir_op_flrp_preserve_kk; break;
   case nir_op_fdiv: alu->op = nir_op_fdiv_preserve_kk; break;
   case nir_op_fsqrt: alu->op = nir_op_fsqrt_preserve_kk; break;
   case nir_op_frsq: alu->op = nir_op_frsq_preserve_kk; break;
   case nir_op_fmin: alu->op = nir_op_fmin_preserve_kk; break;
   case nir_op_fmax: alu->op = nir_op_fmax_preserve_kk; break;
   case nir_op_fsat: alu->op = nir_op_fsat_preserve_kk; break;
   case nir_op_feq: alu->op = nir_op_feq_preserve_kk; break;
   case nir_op_fneu: alu->op = nir_op_fneu_preserve_kk; break;
   case nir_op_flt: alu->op = nir_op_flt_preserve_kk; break;
   case nir_op_fge: alu->op = nir_op_fge_preserve_kk; break;
   case nir_op_ldexp: alu->op = nir_op_ldexp_preserve_kk; break;
   case nir_op_ffloor: alu->op = nir_op_ffloor_preserve_kk; break;
   case nir_op_fceil: alu->op = nir_op_fceil_preserve_kk; break;
   case nir_op_fsign: alu->op = nir_op_fsign_preserve_kk; break;
   case nir_op_fsin: alu->op = nir_op_fsin_preserve_kk; break;
   case nir_op_flog2: alu->op = nir_op_flog2_preserve_kk; break;
   case nir_op_fexp2: alu->op = nir_op_fexp2_preserve_kk; break;
   case nir_op_fpow: alu->op = nir_op_fpow_preserve_kk; break;
   case nir_op_ffract: alu->op = nir_op_ffract_preserve_kk; break;
   case nir_op_frem: alu->op = nir_op_frem_preserve_kk; break;
   case nir_op_frcp: alu->op = nir_op_frcp_preserve_kk; break;
   default: return false;
   }
   /* Comparison/minmax workaround classification uses the original opcode.
    * Preserve its safe call-site mode after selecting the emitter opcode. */
   if (mode && !strcmp(mode, "safe"))
      alu->fp_math_ctrl |= nir_fp_no_contract;
   return true;
}

/* Native FTZ can discard a subnormal before it rounds up to FLT_MIN.
 * Keep only multiply, FMA and divide on a late boundary-repair emitter path;
 * other operations retain their existing native/canonicalization lowering. */
static bool
kk_fp32_flush_instr(UNUSED nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->def.bit_size != 32)
      return false;
   switch (alu->op) {
   case nir_op_fmul: alu->op = nir_op_fmul_preserve_kk; break;
   case nir_op_ffma: alu->op = nir_op_ffma_preserve_kk; break;
   case nir_op_fdiv: alu->op = nir_op_fdiv_preserve_kk; break;
   default: return false;
   }
   return true;
}

static bool
kk_fp32_controls_lower(struct nir_to_msl_ctx *ctx)
{
   unsigned mode = ctx->shader->info.float_controls_execution_mode &
      (FLOAT_CONTROLS_DENORM_PRESERVE_FP32 |
       FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP32);
   if (!mode)
      return false;
   return nir_shader_instructions_pass(ctx->shader,
      mode & FLOAT_CONTROLS_DENORM_PRESERVE_FP32
         ? kk_fp32_preserve_instr : kk_fp32_flush_instr,
      nir_metadata_control_flow, ctx);
}
/* Generic nir_lower_frexp assumes normalized significands. Normalize FP32
 * subnormals with integer bits before that pass; FP16 follows its native path.
 */
static bool kk_fp32_preserve_prepare(nir_builder *b, nir_instr *instr,
                                   UNUSED void *data)
{
   if (instr->type != nir_instr_type_alu)
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
