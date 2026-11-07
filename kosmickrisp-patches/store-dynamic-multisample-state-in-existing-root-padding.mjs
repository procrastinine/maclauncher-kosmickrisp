// SPDX-License-Identifier: MIT
// Store dynamic multisample state in existing root padding
export const id = "store-dynamic-multisample-state-in-existing-root-padding";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.h", source => source
    .replace("         uint64_t attrib_base[KK_MAX_ATTRIBS];\n         uint32_t attrib_clamps[KK_MAX_ATTRIBS];\n         uint64_t vertex_input;\n         uint32_t sample_mask;\n         uint32_t alpha_to_coverage;\n\n         uint64_t fragment_statistic;\n         float blend_constant[4];\n",
      () => "         uint64_t attrib_base[KK_MAX_ATTRIBS];\n         uint32_t attrib_clamps[KK_MAX_ATTRIBS];\n         uint64_t vertex_input;\n\n         uint64_t fragment_statistic;\n         float blend_constant[4];\n")
    .replace("         float viewport_z_range[KK_MAX_VIEWPORTS * 2];\n         bool emulate_depth_clamp;\n         bool emulate_viewport_z;\n      } draw;\n      struct {\n         uint32_t base_group[3];\n",
      () => "         float viewport_z_range[KK_MAX_VIEWPORTS * 2];\n         bool emulate_depth_clamp;\n         bool emulate_viewport_z;\n         uint8_t sample_mask;\n         uint8_t alpha_to_coverage;\n      } draw;\n      struct {\n         uint32_t base_group[3];\n")
    .replace("   uint8_t push[KK_MAX_PUSH_SIZE];\n};\n\nstatic_assert(2u + MESA_ALL_SHADER_STAGES <= KK_MAX_SETS,\n              \"embedded stage bindings fit the existing heap root union\");\nstatic_assert(sizeof(((struct kk_root_descriptor_table *)0)->sets) ==\n",
      () => "   uint8_t push[KK_MAX_PUSH_SIZE];\n};\n\nstatic_assert(KK_MAX_SAMPLES <= 8, \"dynamic sample mask fits the draw tail\");\n\nstatic_assert(2u + MESA_ALL_SHADER_STAGES <= KK_MAX_SETS,\n              \"embedded stage bindings fit the existing heap root union\");\nstatic_assert(sizeof(((struct kk_root_descriptor_table *)0)->sets) ==\n"));
  edit("src/kosmickrisp/vulkan/kk_dynamic_multisample.h", source => source
    .replace("kk_fs_multisample_value(nir_builder *b, uint64_t offset)\n{\n   nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);\n   return nir_load_global_constant(b, 1, 32, nir_iadd_imm(b, root, offset));\n}\n\nstatic void\n",
      () => "kk_fs_multisample_value(nir_builder *b, uint64_t offset)\n{\n   nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);\n   return nir_u2u32(b, nir_load_global_constant(\n      b, 1, 8, nir_iadd_imm(b, root, offset)));\n}\n\nstatic void\n"));
}
