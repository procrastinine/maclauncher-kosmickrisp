// SPDX-License-Identifier: MIT
// Select mesh DGC routes without the software depth-bounds flag
export const id = "select-mesh-dgc-routes-without-the-software-depth";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_dgc_mesh_draw.h", source => source
    .replace("   struct kk_device *dev = kk_cmd_buffer_device(cmd);\n   struct kk_shader *s = cmd->state.gfx.shader;\n   const struct kk_mesh_shader_info *info = &s->mesh->info;\n   if (s->info.dgc_replay || s->info.depth_bounds_emulation || info->flat_heap ||\n       kk_mesh_retained_active(cmd) || cmd->state.gfx.render.layered_bc ||\n       cmd->state.gfx.render.view_mask || s->pipeline.gfx.pre_render_count)\n      return false;\n",
      () => "   struct kk_device *dev = kk_cmd_buffer_device(cmd);\n   struct kk_shader *s = cmd->state.gfx.shader;\n   const struct kk_mesh_shader_info *info = &s->mesh->info;\n   if (s->info.dgc_replay || info->flat_heap ||\n       kk_mesh_retained_active(cmd) || cmd->state.gfx.render.layered_bc ||\n       cmd->state.gfx.render.view_mask || s->pipeline.gfx.pre_render_count)\n      return false;\n"));
}
