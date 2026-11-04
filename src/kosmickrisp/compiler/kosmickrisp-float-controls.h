/* Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 *
 * Canonicalization follows Mesa's agx_nir_lower_fminmax.c at
 * 4f071f891d63396c799b267172c19ba6b0301682 (MIT). Apple min/max and
 * bitwise sign operations can retain denormals even though arithmetic
 * flushes them. Clear the magnitude of zero-exponent results explicitly;
 * Metal may optimize an arithmetic identity even in safe math mode.
 * Bitwise canonicalization retains signed zero, infinities and NaNs. */
#ifndef KOSMICKRISP_FLOAT_CONTROLS_H
#define KOSMICKRISP_FLOAT_CONTROLS_H
static bool
kk_float_control_flush_result(const struct nir_to_msl_ctx *ctx,
                              const nir_alu_instr *alu)
{
   if (alu->def.bit_size != 32 ||
       !nir_is_denorm_flush_to_zero(
          ctx->shader->info.float_controls_execution_mode, 32))
      return false;

   return alu->op == nir_op_fcanonicalize ||
          alu->op == nir_op_fabs || alu->op == nir_op_fneg ||
          alu->op == nir_op_fmin || alu->op == nir_op_fmax ||
          alu->op == nir_op_fsat;
}

static void
kk_float_control_flush_msl(struct nir_to_msl_ctx *ctx,
                           nir_alu_instr *alu)
{
   const char *utype = msl_uint_type(32, alu->def.num_components);
   const char *ftype = msl_type_for_def(ctx->types, &alu->def);
   unsigned id = alu->def.index;
   P_IND(ctx, "t%u = as_type<%s>(select(as_type<%s>(t%u), "
         "as_type<%s>(t%u) & 0x80000000u, "
         "(as_type<%s>(t%u) & 0x7f800000u) == 0u));\n",
         id, ftype, utype, id, utype, id, utype, id);
}
#endif
