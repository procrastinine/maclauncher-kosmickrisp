// SPDX-License-Identifier: MIT
// Form the workaround 10 condition with select
export const id = "form-the-workaround-10-condition-with-select";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/nir_to_msl.c", source => source
    .replace("   case nir_op_bcsel:\n      /* KK_WORKAROUND_10 All shaders will have buf0 bound */\n      if (!(ctx->disabled_workarounds & BITFIELD_BIT(10)) &&\n          ctx->shader->info.stage == MESA_SHADER_COMPUTE && msl_has_buf0(ctx))\n         P(ctx, \"(ulong)&buf0.contents[0] && \");\n      alu_src_to_msl(ctx, instr, 0);\n      P(ctx, \" ? \");\n      alu_src_to_msl(ctx, instr, 1);\n",
      () => "   case nir_op_bcsel:\n      /* KK_WORKAROUND_10 All shaders will have buf0 bound */\n      if (!(ctx->disabled_workarounds & BITFIELD_BIT(10)) &&\n          ctx->shader->info.stage == MESA_SHADER_COMPUTE && msl_has_buf0(ctx)) {\n         if (ctx->buf0_guard && instr->def.num_components == 1) {\n            /* select() forms the guarded condition without the branch that\n             * a short-circuit && produces. */\n            ctx->buf0_guard_used = true;\n            P(ctx, \"select(false, \");\n            alu_src_to_msl(ctx, instr, 0);\n            P(ctx, \", kk_buf0) ? \");\n            alu_src_to_msl(ctx, instr, 1);\n            P(ctx, \" : \");\n            alu_src_to_msl(ctx, instr, 2);\n            break;\n         }\n         P(ctx, \"(ulong)&buf0.contents[0] && \");\n      }\n      alu_src_to_msl(ctx, instr, 0);\n      P(ctx, \" ? \");\n      alu_src_to_msl(ctx, instr, 1);\n"));
}
