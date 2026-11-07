// SPDX-License-Identifier: MIT
// Discard dead locals before MSL declarations
export const id = "discard-dead-locals-before-msl-declarations";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/nir_to_msl.c", source => source
    .replace("   rename_main_entrypoint(shader);\n   kk_float_controls_lower(shader);\n\n   struct nir_to_msl_ctx ctx;\n   memset(&ctx, 0, offsetof(struct nir_to_msl_ctx, mesh));\n   ctx.shader = shader;\n",
      () => "   rename_main_entrypoint(shader);\n   kk_float_controls_lower(shader);\n\n   /* Meta shaders can reach emission without the application optimization\n    * passes. Only surviving deref storage needs a local declaration; dead\n    * aggregate or opaque variables have no scalar MSL storage type. */\n   NIR_PASS(_, shader, nir_remove_dead_variables, nir_var_function_temp, NULL);\n\n   struct nir_to_msl_ctx ctx;\n   memset(&ctx, 0, offsetof(struct nir_to_msl_ctx, mesh));\n   ctx.shader = shader;\n"));
}
