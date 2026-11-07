// SPDX-License-Identifier: MIT
// Match sparse helper return parameter to local pointer width
export const id = "match-sparse-helper-return-parameter-to-local-pointer";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_sparse_buffer_nir.h", source => source
    .replace("      helper->num_params = 7;\n      helper->params = rzalloc_array(helper, nir_parameter, 7);\n      helper->params[0] = (nir_parameter) {\n         .num_components = 1, .bit_size = 64, .mode = nir_parameter_out,\n         .is_return = true, .type = glsl_uint_type(),\n      };\n      for (unsigned i = 1; i < 7; ++i)\n",
      () => "      helper->num_params = 7;\n      helper->params = rzalloc_array(helper, nir_parameter, 7);\n      helper->params[0] = (nir_parameter) {\n         .num_components = 1, .bit_size = nir_get_ptr_bitsize(b->shader),\n         .mode = nir_parameter_out,\n         .is_return = true, .type = glsl_uint_type(),\n      };\n      for (unsigned i = 1; i < 7; ++i)\n"));
}
