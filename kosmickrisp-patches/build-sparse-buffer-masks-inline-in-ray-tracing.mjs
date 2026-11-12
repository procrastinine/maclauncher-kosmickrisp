// SPDX-License-Identifier: MIT
// Build sparse buffer masks inline in ray tracing stages
export const id = "build-sparse-buffer-masks-inline-in-ray-tracing";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_sparse_buffer_nir.h", source => source
    .replace("                      unsigned size, unsigned components, unsigned write_mask,\n                      bool range_lookup)\n{\n   const char *name = \"__kk_sparse_buffer_access\";\n   nir_function *helper = NULL;\n   nir_foreach_function(function, b->shader) {\n",
      () => "                      unsigned size, unsigned components, unsigned write_mask,\n                      bool range_lookup)\n{\n   /* Ray-tracing stages place function-temporary variables in their 64-bit\n    * continuation frames, which cannot be passed as the helper's thread\n    * pointer. Their accesses build the same mask inline. */\n   if (mesa_shader_stage_is_rt(b->shader->info.stage))\n      return kk_sparse_access_mask_body(b, source, address, nir_imm_int(b, size),\n                                        nir_imm_int(b, components),\n                                        nir_imm_int(b, write_mask), range_lookup);\n   const char *name = \"__kk_sparse_buffer_access\";\n   nir_function *helper = NULL;\n   nir_foreach_function(function, b->shader) {\n"));
}
