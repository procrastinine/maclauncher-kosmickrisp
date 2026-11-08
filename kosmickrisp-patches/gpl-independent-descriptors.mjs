// SPDX-License-Identifier: MIT
// Independent layouts can omit preceding sets, so their dynamic prefix is
// resolved from the root table populated when descriptor sets are bound.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s.replace(
    '      unsigned s = i + info->firstSet;\n      VK_FROM_HANDLE(kk_descriptor_set, set, info->pDescriptorSets[i]);',
    '      unsigned s = i + info->firstSet;\n      desc->root.set_dynamic_buffer_start[s] = dyn_buffer_start;\n      VK_FROM_HANDLE(kk_descriptor_set, set, info->pDescriptorSets[i]);'));
}
