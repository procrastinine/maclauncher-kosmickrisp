// SPDX-License-Identifier: MIT
// Lower shader record blocks in ray tracing stages
export const id = "lower-shader-record-blocks-in-ray-tracing-stages";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_rt_cps.c", source => source
    .replace("\n   NIR_PASS(_, shader, nir_lower_var_copies);\n   lower_call_data(shader, &parts->payload_size, &parts->hit_attribute_size);\n   nir_shader_intrinsics_pass(shader, lower_system_value, nir_metadata_none, NULL);\n   NIR_PASS(_, shader, nir_lower_vars_to_ssa);\n   /* Invocation-private arrays that survive a call must live in the same\n",
      () => "\n   NIR_PASS(_, shader, nir_lower_var_copies);\n   lower_call_data(shader, &parts->payload_size, &parts->hit_attribute_size);\n   /* ShaderRecordBufferKHR blocks are constant-memory casts of the 64-bit\n    * record address and use the same global access as the call data. */\n   NIR_PASS(_, shader, nir_lower_explicit_io, nir_var_mem_constant,\n            nir_address_format_64bit_global);\n   nir_shader_intrinsics_pass(shader, lower_system_value, nir_metadata_none, NULL);\n   NIR_PASS(_, shader, nir_lower_vars_to_ssa);\n   /* Invocation-private arrays that survive a call must live in the same\n"));
}
