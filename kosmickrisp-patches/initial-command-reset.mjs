// SPDX-License-Identifier: MIT
// An already initial buffer has no recording to discard again.
export const id = 'initial-command-reset';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', source => source.replace(
    '   kk_reset_cmd_buffer_internal(cmd);\n   vk_command_buffer_reset(&cmd->vk);',
    '   /* Retirement already resets each replay buffer before returning it. */\n' +
    '   if (flags == 0 &&\n' +
    '       cmd->vk.state == MESA_VK_COMMAND_BUFFER_STATE_INITIAL)\n' +
    '      return;\n\n' +
    '   kk_reset_cmd_buffer_internal(cmd);\n   vk_command_buffer_reset(&cmd->vk);'));
}
