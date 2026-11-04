/* SPDX-License-Identifier: MIT */
#pragma once

static const char *
kk_float8_operation(nir_op op)
{
   switch (op) {
   case nir_op_e4m3fn2f: return "decode_e4m3";
   case nir_op_e5m22f: return "decode_e5m2";
   case nir_op_f2e4m3fn: return "encode_e4m3";
   case nir_op_f2e4m3fn_sat: return "encode_e4m3_sat";
   case nir_op_f2e4m3fn_satfn: return "encode_e4m3_satfn";
   case nir_op_f2e5m2: return "encode_e5m2";
   case nir_op_f2e5m2_sat: return "encode_e5m2_sat";
   default: return NULL;
   }
}

static bool
kk_emit_float8_alu(struct nir_to_msl_ctx *ctx, nir_alu_instr *instr)
{
   if (!kk_float8_operation(instr->op))
      return false;
   bool decode = instr->op == nir_op_e4m3fn2f || instr->op == nir_op_e5m22f;
   bool e5m2 = instr->op == nir_op_e5m22f || instr->op == nir_op_f2e5m2 || instr->op == nir_op_f2e5m2_sat;
   bool sat = instr->op == nir_op_f2e4m3fn_sat || instr->op == nir_op_f2e4m3fn_satfn || instr->op == nir_op_f2e5m2_sat;
   P(ctx, decode ? "kk_fp8_decode(uchar(" : "kk_fp8_encode(float(");
   alu_src_to_msl(ctx, instr, 0);
   P(ctx, "), %s", e5m2 ? "true" : "false");
   if (!decode)
      P(ctx, ", %s, %s", sat ? "true" : "false", instr->op == nir_op_f2e4m3fn_satfn ? "true" : "false");
   P(ctx, ")");
   return true;
}

static bool
kk_uses_float8(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu && kk_float8_operation(nir_instr_as_alu(instr)->op))
               return true;
            if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if ((intr->intrinsic == nir_intrinsic_load_cmat_kk && intr->def.bit_size == 8) ||
                   (intr->intrinsic == nir_intrinsic_store_cmat_kk && intr->src[1].ssa->bit_size == 8))
                  return true;
            }
         }
      }
   }
   return false;
}
