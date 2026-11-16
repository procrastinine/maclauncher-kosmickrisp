// SPDX-License-Identifier: MIT
// Adapted from steamac patch 0054 (https://github.com/fxgl/steamac).
// Original author: steamac. Copyright 2026 FX GAMES FZ LLC and patch authors.
// steamac is Apache-2.0; its changes to Mesa follow Mesa MIT, per NOTICE.
// See steamac-NOTICE.txt, steamac-LICENSE.txt and steamac-MIT.txt.
// Hash all graphics state a pipeline's shaders are built from
export const id = "hash-all-graphics-state-a-pipeline-s-shaders-are";
export const sourceFiles = ['kosmickrisp-patches/steamac-licenses.mjs', 'kosmickrisp-patches/steamac-NOTICE.txt', 'kosmickrisp-patches/steamac-LICENSE.txt', 'kosmickrisp-patches/steamac-MIT.txt'];
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("   uint8_t depth_input_att;\n   uint8_t stencil_input_att;\n};\n\nstatic void\n",
      () => "   uint8_t depth_input_att;\n   uint8_t stencil_input_att;\n   /* gather_graphics_pipeline_create_info builds the native pipeline's\n    * attachment formats from these, so equal keys need equal attachments,\n    * not only the presence of depth and stencil. */\n   VkFormat depth_format;\n   VkFormat stencil_format;\n   uint32_t color_attachment_count;\n   uint8_t color_map[MESA_VK_MAX_COLOR_ATTACHMENTS];\n};\n\nstatic inline uint8_t\nkk_get_logical_color_att_index(const struct vk_graphics_pipeline_state *state,\n                               uint8_t i);\nstatic bool\nkk_is_possible_both_depth_clip_clamp(\n   const struct vk_graphics_pipeline_state *state, bool enabled);\n\nstatic void\n")
    .replace("   key->dynamic_input_attachment_map =\n      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_INPUT_ATTACHMENT_MAP);\n}\n\n",
      () => "   key->dynamic_input_attachment_map =\n      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_INPUT_ATTACHMENT_MAP);\n\n   if (state->rp) {\n      key->depth_format = state->rp->depth_attachment_format;\n      key->stencil_format = state->rp->stencil_attachment_format;\n      key->color_attachment_count = state->rp->color_attachment_count;\n   }\n   for (uint8_t i = 0; i < MESA_VK_MAX_COLOR_ATTACHMENTS; i++)\n      key->color_map[i] = kk_get_logical_color_att_index(state, i);\n}\n\n")
    .replace("      const uint32_t view_mask = state->mv ? state->mv->view_mask : 0;\n      _mesa_blake3_update(&blake3_ctx, &view_mask, sizeof(view_mask));\n   }\n\n",
      () => "      const uint32_t view_mask = state->mv ? state->mv->view_mask : 0;\n      _mesa_blake3_update(&blake3_ctx, &view_mask, sizeof(view_mask));\n   }\n\n   /* Vertex and fragment lowering add depth clip and clamp emulation by\n    * whether the rasterization state lets both be disabled or both be\n    * enabled, so that answer is part of every graphics stage's key. */\n   if (state && (stages & (VK_SHADER_STAGE_ALL_GRAPHICS |\n                           VK_SHADER_STAGE_TASK_BIT_EXT |\n                           VK_SHADER_STAGE_MESH_BIT_EXT))) {\n      const uint32_t clip_clamp_key[] = {\n         0x44434c50u,\n         kk_is_possible_both_depth_clip_clamp(state, false),\n         kk_is_possible_both_depth_clip_clamp(state, true),\n      };\n      _mesa_blake3_update(&blake3_ctx, clip_clamp_key, sizeof(clip_clamp_key));\n   }\n\n"));
}
