// SPDX-License-Identifier: MIT
// Restrict subgroup stages to supported tessellation paths
export const id = "restrict-subgroup-stages-to-supported-tessellation-paths";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("                                 VK_SHADER_STAGE_FRAGMENT_BIT |\n                                 VK_SHADER_STAGE_VERTEX_BIT |\n                                 VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |\n                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |\n                                 VK_SHADER_STAGE_GEOMETRY_BIT,\n      .subgroupSupportedOperations =\n         VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |\n",
      () => "                                 VK_SHADER_STAGE_FRAGMENT_BIT |\n                                 VK_SHADER_STAGE_VERTEX_BIT |\n                                 VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |\n                                 VK_SHADER_STAGE_GEOMETRY_BIT,\n      .subgroupSupportedOperations =\n         VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |\n"));
}
