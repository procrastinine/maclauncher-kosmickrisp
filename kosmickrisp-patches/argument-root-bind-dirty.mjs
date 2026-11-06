// SPDX-License-Identifier: MIT
// Preserve the root argument entry across unchanged draws and dispatches.
export const id = 'argument-root-bind-dirty';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', source => source.replace(
    '      uint64_t root_addr;',
    '      uint64_t root_addr;\n      mtl_argument_table *root_table;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', source => source.replace(
    '   mtl_set_address(cmd->argument_table, addr, KK_ARGUMENT_ROOT);\n   cmd->state.root_addr = addr;',
    '   /* Internal precompiled dispatches restore root_addr before returning.\n' +
    '    * A detached interlock argument table still needs its own first bind. */\n' +
    '   if (cmd->state.root_table == cmd->argument_table &&\n' +
    '       cmd->state.root_addr == addr)\n' +
    '      return;\n' +
    '   mtl_set_address(cmd->argument_table, addr, KK_ARGUMENT_ROOT);\n' +
    '   cmd->state.root_addr = addr;\n' +
    '   cmd->state.root_table = cmd->argument_table;'));
}
