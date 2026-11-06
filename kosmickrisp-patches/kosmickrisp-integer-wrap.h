/* SPDX-License-Identifier: MIT */
/* NIR arithmetic wraps at its result width. Metal signed overflow is undefined. */
static void
kk_msl_wrap_source(struct nir_to_msl_ctx *ctx, nir_alu_instr *instr, unsigned src)
{
   unsigned bits = instr->def.bit_size;
   unsigned components = instr->def.num_components;
   if (bits < 32)
      P(ctx, "%s(", msl_uint_type(32, components));
   P(ctx, "as_type<%s>(", msl_uint_type(bits, components));
   alu_src_to_msl(ctx, instr, src);
   P(ctx, ")");
   if (bits < 32)
      P(ctx, ")");
}

static bool
kk_msl_integer_wrap(struct nir_to_msl_ctx *ctx, nir_alu_instr *instr)
{
   const char *op;
   switch (instr->op) {
   case nir_op_iadd: case nir_op_iadd3: op = " + "; break;
   case nir_op_isub: op = " - "; break;
   case nir_op_imul: case nir_op_imad: op = " * "; break;
   case nir_op_ineg: op = NULL; break;
   default: return false;
   }
   unsigned bits = instr->def.bit_size;
   assert(bits == 8 || bits == 16 || bits == 32 || bits == 64);
   P(ctx, "as_type<%s>(", msl_type_for_def(ctx->types, &instr->def));
   /* C++ promotes scalar uchar/ushort to signed int. Promote to uint first,
    * then narrow the unsigned result before reinterpreting its exact bits. */
   if (bits < 32)
      P(ctx, "%s(", msl_uint_type(bits, instr->def.num_components));
   if (instr->op == nir_op_ineg) {
      P(ctx, "%s(0) - ", msl_uint_type(MAX2(bits, 32), instr->def.num_components));
      kk_msl_wrap_source(ctx, instr, 0);
   } else {
      kk_msl_wrap_source(ctx, instr, 0);
      P(ctx, "%s", op);
      kk_msl_wrap_source(ctx, instr, 1);
      if (instr->op == nir_op_imad || instr->op == nir_op_iadd3) {
         P(ctx, " + ");
         kk_msl_wrap_source(ctx, instr, 2);
      }
   }
   if (bits < 32)
      P(ctx, ")");
   P(ctx, ")");
   return true;
}
