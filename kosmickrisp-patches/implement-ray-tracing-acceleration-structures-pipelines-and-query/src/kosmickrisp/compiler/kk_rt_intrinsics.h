/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_INTRINSICS_H
#define KK_RT_INTRINSICS_H
static void
kk_rt_parameter_to_msl(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *in)
{
   assert(mesa_shader_stage_is_rt(ctx->shader->info.stage));
   assert(nir_intrinsic_param_idx(in) == 0 && in->def.bit_size == 64 &&
          in->def.num_components == 1);
   P(ctx, "(ulong)kk_rt_context;\n");
}
#endif
