// SPDX-License-Identifier: MIT
// Set shader module identifiers once in the device tables
export const id = "set-shader-module-identifiers-once-in-the-device";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("      .EXT_sample_locations = true,\n      /* Apple7 support is the image_atomic64 registry entry. */\n      .EXT_shader_image_atomic_int64 = true,\n      .EXT_shader_module_identifier = true,\n      .VALVE_shader_mixed_float_dot_product = true,\n      .EXT_shader_atomic_float = true,\n      .EXT_shader_replicated_composites = true,\n",
      () => "      .EXT_sample_locations = true,\n      /* Apple7 support is the image_atomic64 registry entry. */\n      .EXT_shader_image_atomic_int64 = true,\n      .VALVE_shader_mixed_float_dot_product = true,\n      .EXT_shader_atomic_float = true,\n      .EXT_shader_replicated_composites = true,\n")
    .replace("      .shaderModuleIdentifier = true,\n      .graphicsPipelineLibrary = true,\n      .pipelineCreationCacheControl = true,\n      .shaderModuleIdentifier = true,\n      .shaderMixedFloatDotProductFloat16AccFloat32 = true,\n      .privateData = true,\n      .robustImageAccess = true,\n",
      () => "      .shaderModuleIdentifier = true,\n      .graphicsPipelineLibrary = true,\n      .pipelineCreationCacheControl = true,\n      .shaderMixedFloatDotProductFloat16AccFloat32 = true,\n      .privateData = true,\n      .robustImageAccess = true,\n"));
}
