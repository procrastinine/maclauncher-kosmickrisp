// SPDX-License-Identifier: MIT
// Begin the depth-bounds transaction on generated Mesh sequences
export const id = "begin-the-depth-bounds-transaction-on-generated-mesh";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_dgc_mesh_draw.h", source => source
    .replace("   uint64_t saved_address = gfx->descriptors.root.addr;\n   gfx->generated_root = root;\n   gfx->descriptors.root_dirty = true;\n   kk_draw_mesh(cmd, (struct mtl_size){0}, args, 0, 1, 12, false);\n   gfx->generated_root = saved_root;\n   gfx->descriptors.root.addr = saved_address;\n   gfx->descriptors.root_dirty = true;\n",
      () => "   uint64_t saved_address = gfx->descriptors.root.addr;\n   gfx->generated_root = root;\n   gfx->descriptors.root_dirty = true;\n   /* This route calls the Mesh draw directly, not through the dispatch\n    * table, so it begins the software depth-bounds transaction itself. */\n   int bounds = kk_depth_bounds_mesh_begin(kk_cmd_buffer_to_handle(cmd));\n   if (bounds >= 0) {\n      kk_draw_mesh(cmd, (struct mtl_size){0}, args, 0, 1, 12, false);\n      if (bounds) kk_mesh_retained_transaction_end(cmd);\n   }\n   gfx->generated_root = saved_root;\n   gfx->descriptors.root.addr = saved_address;\n   gfx->descriptors.root_dirty = true;\n"));
}
