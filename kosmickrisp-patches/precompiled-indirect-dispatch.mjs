// SPDX-License-Identifier: MIT
export function apply({edit}) {
// Precompiled kernels can consume either indirect record format, just like
  // kk_dispatch_compute. An indirect thread grid must not become a group grid.
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s.replace(
    `   if (grid.mode == KK_GRID_DIRECT)
      mtl_dispatch_threads(encoder, grid.size, local_size);
   else
      mtl_dispatch_threadgroups_with_indirect_buffer(encoder, grid.addr,
                                                     local_size);`,
    `   if (grid.mode == KK_GRID_DIRECT)
      mtl_dispatch_threads(encoder, grid.size, local_size);
   else if (grid.mode == KK_GRID_INDIRECT_LOCAL)
      mtl_dispatch_threads_with_indirect_buffer(encoder, grid.addr);
   else
      mtl_dispatch_threadgroups_with_indirect_buffer(encoder, grid.addr,
                                                     local_size);`));
}
