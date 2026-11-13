// SPDX-License-Identifier: MIT
// Keep the begin-rendering clear outside conditional rendering
export const id = "keep-the-begin-rendering-clear-outside-conditional-rendering";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_draw.c", source => source
    .replace("         .layerCount = render->view_mask ? 1 : render->layer_count,\n      };\n\n      kk_CmdClearAttachments(kk_cmd_buffer_to_handle(cmd), clear_count,\n                             clear_att, 1, &clear_rect);\n   }\n",
      () => "         .layerCount = render->view_mask ? 1 : render->layer_count,\n      };\n\n      /* This is the attachment load operation, which conditional rendering\n       * never discards, unlike an application vkCmdClearAttachments. */\n      const struct kk_conditional_rendering_state cond_render =\n         cmd->state.cond_render;\n      cmd->state.cond_render.enabled = false;\n      kk_CmdClearAttachments(kk_cmd_buffer_to_handle(cmd), clear_count,\n                             clear_att, 1, &clear_rect);\n   }\n")
    .replace("      cmd->state.cond_render.enabled = false;\n      kk_CmdClearAttachments(kk_cmd_buffer_to_handle(cmd), clear_count,\n                             clear_att, 1, &clear_rect);\n   }\n}\n\n",
      () => "      cmd->state.cond_render.enabled = false;\n      kk_CmdClearAttachments(kk_cmd_buffer_to_handle(cmd), clear_count,\n                             clear_att, 1, &clear_rect);\n      cmd->state.cond_render = cond_render;\n   }\n}\n\n"));
}
