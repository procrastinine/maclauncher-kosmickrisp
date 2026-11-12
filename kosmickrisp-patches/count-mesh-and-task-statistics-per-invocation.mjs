// SPDX-License-Identifier: MIT
// Count Mesh and task statistics per invocation
export const id = "count-mesh-and-task-statistics-per-invocation";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/kk_mesh_msl.h", source => source
    .replace("   /* Function temporaries, such as sparse-access residency results, are\n    * declared in the body as in the ordinary entry points. */\n   msl_helper_locals(ctx, impl);\n   if (ctx->mesh.queries)\n      P_IND(ctx, \"if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, 1);\\n\", task ? \"task\" : \"mesh\");\n   predeclare_ssa_values(ctx, impl);\n",
      () => "   /* Function temporaries, such as sparse-access residency results, are\n    * declared in the body as in the ordinary entry points. */\n   msl_helper_locals(ctx, impl);\n   /* The statistics count invocations: one atomic per workgroup adds its\n    * local invocation count. */\n   if (ctx->mesh.queries)\n      P_IND(ctx, \"if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, 1);\\n\", task ? \"task\" : \"mesh\");\n   predeclare_ssa_values(ctx, impl);\n")
    .replace("   /* The statistics count invocations: one atomic per workgroup adds its\n    * local invocation count. */\n   if (ctx->mesh.queries)\n      P_IND(ctx, \"if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, 1);\\n\", task ? \"task\" : \"mesh\");\n   predeclare_ssa_values(ctx, impl);\n   foreach_list_typed(nir_cf_node, node, node, &impl->body)\n      cf_node_to_metal(ctx, node);\n",
      () => "   /* The statistics count invocations: one atomic per workgroup adds its\n    * local invocation count. */\n   if (ctx->mesh.queries)\n      P_IND(ctx, \"if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, %uu);\\n\",\n            task ? \"task\" : \"mesh\", MAX2(threads, 1u));\n   predeclare_ssa_values(ctx, impl);\n   foreach_list_typed(nir_cf_node, node, node, &impl->body)\n      cf_node_to_metal(ctx, node);\n"));
}
