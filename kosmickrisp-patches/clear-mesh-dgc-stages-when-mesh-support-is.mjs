// SPDX-License-Identifier: MIT
// Clear mesh DGC stages when mesh support is disabled
export const id = "clear-mesh-dgc-stages-when-mesh-support-is";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_emulation.json", source => source
    .replace("    \"performance\": true,\n    \"performance_reason\": \"vkd3d-proton requires both meshShader and taskShader for D3D12 MeshTier1; disabling them removes feature-level 12.2 eligibility.\",\n    \"off_consequence\": \"D3D12 MeshTier1 and feature-level 12.2 are unavailable; mesh/task Vulkan shaders and mesh queries are unavailable. Lower feature levels remain subject to their other requirements; this entry does not remove a shader-model requirement or a translator device-creation prerequisite.\",\n    \"native_from\": null\n  },\n  {\n    \"name\": \"polygon_points\",\n",
      () => "    \"performance\": true,\n    \"performance_reason\": \"vkd3d-proton requires both meshShader and taskShader for D3D12 MeshTier1; disabling them removes feature-level 12.2 eligibility.\",\n    \"off_consequence\": \"D3D12 MeshTier1 and feature-level 12.2 are unavailable; mesh/task Vulkan shaders and mesh queries are unavailable. Lower feature levels remain subject to their other requirements; this entry does not remove a shader-model requirement or a translator device-creation prerequisite.\",\n    \"native_from\": null,\n    \"property_bits\": {\n      \"supportedIndirectCommandsShaderStages\": \"VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT\"\n    }\n  },\n  {\n    \"name\": \"polygon_points\",\n"));
}
