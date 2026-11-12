// SPDX-License-Identifier: MIT
// Declare function temporaries in Mesh shader bodies
export const id = "declare-function-temporaries-in-mesh-shader-bodies";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/kk_mesh_msl.h", source => source
    .replace("   ctx->indentlevel++;\n   kk_mesh_emit_aliases(ctx);\n   kk_mesh_emit_locals(ctx);\n   if (ctx->mesh.queries)\n      P_IND(ctx, \"if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, 1);\\n\", task ? \"task\" : \"mesh\");\n   predeclare_ssa_values(ctx, impl);\n",
      () => "   ctx->indentlevel++;\n   kk_mesh_emit_aliases(ctx);\n   kk_mesh_emit_locals(ctx);\n   /* Function temporaries, such as sparse-access residency results, are\n    * declared in the body as in the ordinary entry points. */\n   msl_helper_locals(ctx, impl);\n   if (ctx->mesh.queries)\n      P_IND(ctx, \"if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, 1);\\n\", task ? \"task\" : \"mesh\");\n   predeclare_ssa_values(ctx, impl);\n"));
}
