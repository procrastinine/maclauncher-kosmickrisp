// SPDX-License-Identifier: MIT
// Apply conditional rendering to indirect byte-count draws
export const id = "apply-conditional-rendering-to-indirect-byte-count-draws";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_draw.c", source => source
    .replace("      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),\n      .upload_mask = build_per_draw_upload_mask(cmd),\n      .draw_count = 1, .indirect = true,\n      .indirect_command = { .addr = draw, .stride = sizeof(VkDrawIndirectCommand) },\n   };\n   kk_draw(cmd, &data);\n",
      () => "      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),\n      .upload_mask = build_per_draw_upload_mask(cmd),\n      .draw_count = 1, .indirect = true,\n      .predicate_count = cmd->state.cond_render.enabled ? 1u : 0u,\n      .predicate_op[0] = cmd->state.cond_render.inverted\n                            ? KK_PREDICATE_EQ_ZERO\n                            : KK_PREDICATE_NEQ_ZERO,\n      .predicate_addr[0] = cmd->state.cond_render.address,\n      .indirect_command = { .addr = draw, .stride = sizeof(VkDrawIndirectCommand) },\n   };\n   kk_draw(cmd, &data);\n"));
}
