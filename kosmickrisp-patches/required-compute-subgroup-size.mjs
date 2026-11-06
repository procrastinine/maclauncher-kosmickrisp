// SPDX-License-Identifier: MIT
/** The existing fixed-width compute path implements explicit wave32 requests. */
export const id = 'required-compute-subgroup-size';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '      .requiredSubgroupSizeStages = 0,',
    '      .requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT,'));
}
