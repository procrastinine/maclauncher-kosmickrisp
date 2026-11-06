// SPDX-License-Identifier: MIT
export const id = 'dgc-linked-table-buffers';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    '   bool dgc_icb;',
    '   bool dgc_icb;\n   uint64_t dgc_linked_vs_buffer, dgc_linked_fs_buffer;'));
  edit('src/kosmickrisp/vulkan/kk_dgc.h', s => s
    .replace('#include <stdbool.h>', '#include <assert.h>\n#include <stdbool.h>')
    .replace('   uint32_t vertex_fetch_size, vertex_fetch_stride;\n};',
      '   uint32_t vertex_fetch_size, vertex_fetch_stride;\n   uint64_t linked_vs_buffer, linked_fs_buffer;\n   uint64_t interlock_guard;\n};\nstatic_assert(sizeof(struct kk_dgc_params) == 248, "DGC parameter ABI");')
    .replace('void kk_dgc_release_commands(struct kk_cmd_buffer *cmd);',
      'void kk_dgc_set_linked_table_buffers(struct kk_cmd_buffer *cmd,\n   uint64_t vertex_buffer, uint64_t fragment_buffer);\nvoid kk_dgc_release_commands(struct kk_cmd_buffer *cmd);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_dgc.c', s => s
    .replace('static void kk_dgc_set_graphics_indirect(struct kk_cmd_buffer *cmd, bool indirect)',
`void kk_dgc_set_linked_table_buffers(struct kk_cmd_buffer *cmd,
   uint64_t vertex_buffer, uint64_t fragment_buffer)
{
   cmd->state.gfx.dgc_linked_vs_buffer = vertex_buffer;
   cmd->state.gfx.dgc_linked_fs_buffer = fragment_buffer;
}

static void kk_dgc_set_graphics_indirect(struct kk_cmd_buffer *cmd, bool indirect)`)
    .replace('   for (uint32_t first = 0; first < maximum; first += KK_DGC_ICB_SLICE) {',
`   if (!compute && !poly) {
      kk_dgc_set_graphics_indirect(cmd, true);
      kk_dgc_flush_graphics(cmd);
      p.linked_vs_buffer = cmd->state.gfx.dgc_linked_vs_buffer;
      p.linked_fs_buffer = cmd->state.gfx.dgc_linked_fs_buffer;
      p.interlock_guard = dev->interlock_guard ? dev->interlock_guard->gpu : 0;
   }
   for (uint32_t first = 0; first < maximum; first += KK_DGC_ICB_SLICE) {`));
  edit('src/kosmickrisp/vulkan/kk_dgc.m', s => s.replace(
    '         (struct kk_argument_table_usage){0});\n      desc.maxVertexBufferBindCount',
    '         (struct kk_argument_table_usage){\n            .linked_graphics = !compute,\n            .interlock_guard = !compute && dev->interlock_guard != NULL,\n         });\n      desc.maxVertexBufferBindCount'));
  const escaped = s => JSON.stringify(s).slice(1, -1);
  edit('src/kosmickrisp/vulkan/kk_dgc_shader.h', s => s
    .replace(escaped('   uint vertex_fetch_size, vertex_fetch_stride;\n};'),
      escaped('   uint vertex_fetch_size, vertex_fetch_stride;\n   ulong linked_vs_buffer, linked_fs_buffer;\n   ulong interlock_guard;\n};\nstatic_assert(sizeof(Params) == 248, "DGC parameter ABI");'))
    .replace(escaped('      cmd.set_vertex_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW); cmd.set_fragment_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW);'),
      escaped('      cmd.set_vertex_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW); cmd.set_fragment_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW);\n      cmd.set_vertex_buffer((device void *)p.linked_vs_buffer,KK_ARGUMENT_VS_VFT_WRAPPER);\n      cmd.set_fragment_buffer((device void *)p.linked_fs_buffer,KK_ARGUMENT_FS_VFT_WRAPPER);\n      if (p.interlock_guard)\n         cmd.set_fragment_buffer((device void *)p.interlock_guard,KK_ARGUMENT_INTERLOCK_GUARD);')));
}
