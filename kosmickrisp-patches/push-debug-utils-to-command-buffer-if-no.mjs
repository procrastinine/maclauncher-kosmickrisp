// SPDX-License-Identifier: MIT
// Push debug utils to command buffer if no encoder is active
export const id = "push-debug-utils-to-command-buffer-if-no";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.c", source => source
    .replace("\n   if (cmd->metal.render)\n      mtl_encoder_push_debug_group(cmd->metal.render, pLabelInfo->pLabelName);\n\n   if (cmd->metal.compute)\n      mtl_encoder_push_debug_group(cmd->metal.compute, pLabelInfo->pLabelName);\n}\n\n",
      () => "\n   if (cmd->metal.render)\n      mtl_encoder_push_debug_group(cmd->metal.render, pLabelInfo->pLabelName);\n   else if (cmd->metal.compute)\n      mtl_encoder_push_debug_group(cmd->metal.compute, pLabelInfo->pLabelName);\n}\n\n")
    .replace("      mtl_encoder_push_debug_group(cmd->metal.render, pLabelInfo->pLabelName);\n   else if (cmd->metal.compute)\n      mtl_encoder_push_debug_group(cmd->metal.compute, pLabelInfo->pLabelName);\n}\n\nVKAPI_ATTR void VKAPI_CALL\n",
      () => "      mtl_encoder_push_debug_group(cmd->metal.render, pLabelInfo->pLabelName);\n   else if (cmd->metal.compute)\n      mtl_encoder_push_debug_group(cmd->metal.compute, pLabelInfo->pLabelName);\n   else\n      mtl_command_buffer_push_debug_group(cmd->metal.cmd_buf,\n                                          pLabelInfo->pLabelName);\n}\n\nVKAPI_ATTR void VKAPI_CALL\n")
    .replace("\n   if (cmd->metal.render)\n      mtl_encoder_pop_debug_group(cmd->metal.render);\n\n   if (cmd->metal.compute)\n      mtl_encoder_pop_debug_group(cmd->metal.compute);\n}\n\n",
      () => "\n   if (cmd->metal.render)\n      mtl_encoder_pop_debug_group(cmd->metal.render);\n   else if (cmd->metal.compute)\n      mtl_encoder_pop_debug_group(cmd->metal.compute);\n}\n\n")
    .replace("      mtl_encoder_pop_debug_group(cmd->metal.render);\n   else if (cmd->metal.compute)\n      mtl_encoder_pop_debug_group(cmd->metal.compute);\n}\n\nVKAPI_ATTR void VKAPI_CALL\n",
      () => "      mtl_encoder_pop_debug_group(cmd->metal.render);\n   else if (cmd->metal.compute)\n      mtl_encoder_pop_debug_group(cmd->metal.compute);\n   else\n      mtl_command_buffer_pop_debug_group(cmd->metal.cmd_buf);\n}\n\nVKAPI_ATTR void VKAPI_CALL\n"));
}
