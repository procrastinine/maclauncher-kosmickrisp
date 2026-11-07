// SPDX-License-Identifier: MIT
// Preserve sampler specialization requirements when linking stages
export const id = "preserve-sampler-specialization-requirements-when-linking-stages";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("static void\nkk_msl_copy(struct msl_compile_data *dst, const struct msl_compile_data *src)\n{\n   dst->code = ralloc_strdup(NULL, src->code);\n   dst->entrypoint_name = ralloc_strdup(NULL, src->entrypoint_name);\n   dst->requires_msl41 = src->requires_msl41;\n   dst->uses_matrix_coordinates = src->uses_matrix_coordinates;\n   dst->uses_atomic64_locks = src->uses_atomic64_locks;\n   dst->uses_pipeline_statistics = src->uses_pipeline_statistics;\n}\n\n/* Copies all msl shader data to the vertex and gathers information for pipeline\n",
      () => "static void\nkk_msl_copy(struct msl_compile_data *dst, const struct msl_compile_data *src)\n{\n   /* Requirements belong to the source, including stage specialization.\n    * Copy them together so new compiler outputs survive graphics linking. */\n   *dst = *src;\n   dst->code = ralloc_strdup(NULL, src->code);\n   dst->entrypoint_name = ralloc_strdup(NULL, src->entrypoint_name);\n}\n\n/* Copies all msl shader data to the vertex and gathers information for pipeline\n"));
}
