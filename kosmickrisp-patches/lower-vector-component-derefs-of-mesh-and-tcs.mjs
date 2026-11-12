// SPDX-License-Identifier: MIT
// Lower vector component derefs of Mesh and TCS outputs
export const id = "lower-vector-component-derefs-of-mesh-and-tcs";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,\n            kk_buffer_addr_format(rs->uniform_buffers));\n\n   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,\n            type_size_vec4,\n            nir_lower_io_lower_64bit_to_32 |\n",
      () => "   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,\n            kk_buffer_addr_format(rs->uniform_buffers));\n\n   /* Mesh and tessellation control outputs are shared between invocations,\n    * so SPIR-V component accesses reach them as array derefs of a vector;\n    * nir_lower_io would read those indices as slot offsets. */\n   if (nir->info.stage == MESA_SHADER_MESH ||\n       nir->info.stage == MESA_SHADER_TESS_CTRL)\n      NIR_PASS(_, nir, nir_lower_array_deref_of_vec, nir_var_shader_out, NULL,\n               nir_lower_direct_array_deref_of_vec_load |\n                  nir_lower_indirect_array_deref_of_vec_load |\n                  nir_lower_direct_array_deref_of_vec_store |\n                  nir_lower_indirect_array_deref_of_vec_store);\n\n   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,\n            type_size_vec4,\n            nir_lower_io_lower_64bit_to_32 |\n"));
}
