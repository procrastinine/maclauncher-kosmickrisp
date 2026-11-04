/* Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 *
 * Canonicalization follows Mesa's agx_nir_lower_fminmax.c at
 * 4f071f891d63396c799b267172c19ba6b0301682 (MIT). Apple min/max and
 * bitwise sign operations can retain denormals even though arithmetic
 * flushes them. Clear zero-exponent magnitudes with integer operations.
 */
#ifndef KOSMICKRISP_FLOAT_CONTROLS_H
#define KOSMICKRISP_FLOAT_CONTROLS_H

static bool
kk_float_control_flush(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->def.bit_size != 32)
      return false;

   switch (alu->op) {
   case nir_op_fcanonicalize:
   case nir_op_fabs:
   case nir_op_fneg:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_fsat:
      break;
   default:
      return false;
   }

   /* Run after algebraic optimization so no subsequent float identity can
    * remove this canonicalization or introduce another unflushed operation.
    * Keep the original operation in safe math mode, as in the emitter path.
    */
   alu->fp_math_ctrl |= nir_fp_exact;
   b->cursor = nir_after_instr(instr);
   nir_def *exponent = nir_iand_imm(b, &alu->def, 0x7f800000);
   /* The exponent is in [0, INT32_MAX]. Its negation has a sign bit exactly
    * when the exponent is nonzero. This mask avoids a float operation or a
    * select (and the compute-select compiler workaround) in the flush path.
    */
   nir_def *mask = nir_ishr_imm(b, nir_ineg(b, exponent), 31);
   mask = nir_ior_imm(b, mask, 0x80000000);
   nir_def *result = nir_iand(b, &alu->def, mask);
   nir_def_rewrite_uses_after(&alu->def, result);
   return true;
}

static void
kk_float_controls_lower(nir_shader *shader)
{
   if (nir_is_denorm_flush_to_zero(
          shader->info.float_controls_execution_mode, 32)) {
      NIR_PASS(_, shader, nir_shader_instructions_pass, kk_float_control_flush,
               nir_metadata_control_flow, NULL);
   }
}
#endif
