// SPDX-License-Identifier: MIT
// Declare local variables in ray tracing visible functions
export const id = "declare-local-variables-in-ray-tracing-visible-functions";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/kk_rt_msl.h", source => source
    .replace("      offsetof(struct kk_rt_cps_context, lane));\n   P_IND(ctx, \"uint gl_SubGroupSize = *(device uint*)(kk_rt_context + %zu);\\n\",\n      offsetof(struct kk_rt_cps_context, subgroup_size));\n   predeclare_ssa_values(ctx, impl);\n   foreach_list_typed(nir_cf_node, node, node, &impl->body)\n      cf_node_to_metal(ctx, node);\n",
      () => "      offsetof(struct kk_rt_cps_context, lane));\n   P_IND(ctx, \"uint gl_SubGroupSize = *(device uint*)(kk_rt_context + %zu);\\n\",\n      offsetof(struct kk_rt_cps_context, subgroup_size));\n   /* Values that live across a call are already in the continuation frame.\n    * Arrays that later lowering places in scratch are local to this part. */\n   emit_local_vars(ctx, impl->function->shader);\n   predeclare_ssa_values(ctx, impl);\n   foreach_list_typed(nir_cf_node, node, node, &impl->body)\n      cf_node_to_metal(ctx, node);\n"));
}
