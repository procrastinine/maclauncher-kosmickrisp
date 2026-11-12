// SPDX-License-Identifier: MIT
// Defer multiview amplification until pipeline selection
export const id = "defer-multiview-amplification-until-pipeline-selection";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.c", source => source
    .replace("\n   {\n      KK_PERF_ENCODER_SCOPE(kk_cmd_buffer_device(cmd), KK_PERF_SETUP_BINDS, binds);\n      /* Pipeline binding may occur after BeginRendering. Force first apply. */\n      state->amplification_view_mask = ~state->render.view_mask;\n      kk_cmd_buffer_apply_view_mask(cmd);\n      /* Fresh encoders have no table state, even when Metal reuses a pointer.\n       * Bind once here; graphics state invalidation remains per encoder. */\n      mtl_render_set_argument_table(cmd->metal.render, cmd->argument_table,\n",
      () => "\n   {\n      KK_PERF_ENCODER_SCOPE(kk_cmd_buffer_device(cmd), KK_PERF_SETUP_BINDS, binds);\n      /* BeginRendering may precede Mesh binding. Its Vulkan view count can\n       * exceed native amplification (32 Mesh views versus 8 on Apple7/8).\n       * Start with one view; the pipeline flush applies the selected map. */\n      uint32_t layer = 0;\n      mtl_set_vertex_amplification_count(cmd->metal.render, &layer, 1);\n      state->amplification_view_mask = 0;\n      /* Fresh encoders have no table state, even when Metal reuses a pointer.\n       * Bind once here; graphics state invalidation remains per encoder. */\n      mtl_render_set_argument_table(cmd->metal.render, cmd->argument_table,\n"));
}
