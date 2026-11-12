// SPDX-License-Identifier: MIT
// Keep dynamic-rate pipelines with flat inputs on the fine route
export const id = "keep-dynamic-rate-pipelines-with-flat-inputs-on";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("      !infos[0].nir->xfb_info && !infos[0].nir->info.cull_distance_array_size &&\n      !(infos[0].flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT) &&\n      !depth_bounds && !pixel_candidate && !polygon_capture && !poly_layer_replay &&\n      !barycentric_inputs && !kk_polygon_native_possible(state) &&\n      !kk_depth_bias_native_possible(depth_bias_control, state);\n   nir_shader *coarse_fs = NULL;\n   for (uint32_t step = 0u; step < shader_count; ++step) {\n",
      () => "      !infos[0].nir->xfb_info && !infos[0].nir->info.cull_distance_array_size &&\n      !(infos[0].flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT) &&\n      !depth_bounds && !pixel_candidate && !polygon_capture && !poly_layer_replay &&\n      !(barycentric_inputs & (KK_BARYCENTRIC_INPUT_PER_VERTEX | KK_BARYCENTRIC_INPUT_COORD)) &&\n      !kk_polygon_native_possible(state) &&\n      !kk_depth_bias_native_possible(depth_bias_control, state);\n   nir_shader *coarse_fs = NULL;\n   for (uint32_t step = 0u; step < shader_count; ++step) {\n"));
}
