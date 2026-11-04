/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SOURCE_COMMAND_H
#define KK_INTERLOCK_SOURCE_COMMAND_H

static unsigned
kk_interlock_source_arity(struct kk_cmd_buffer *cmd)
{
   return mesa_vertices_per_prim(kk_geometry_shader(cmd)->info.geometry.mode);
}

static void
kk_interlock_source_setup(struct kk_cmd_buffer *cmd, uint64_t params)
{
   libkk_interlock_source_capture_setup(cmd, kk_grid_1d(1), true,
      params, kk_heap(cmd), kk_interlock_source_arity(cmd));
}

static struct kk_grid
kk_interlock_source_grid(struct kk_cmd_buffer *cmd)
{
   return kk_grid_indirect(cmd->state.gfx.polygon_capture +
      offsetof(struct kk_interlock_envelope, source_dispatch));
}

static struct kk_draw_data
kk_interlock_source_assemble(struct kk_cmd_buffer *cmd, uint64_t params)
{
   unsigned arity = kk_interlock_source_arity(cmd);
   uint64_t capture = cmd->state.gfx.polygon_capture;
   struct kk_grid grid = kk_interlock_source_grid(cmd);
   libkk_interlock_source_clip(cmd, grid, true, params, kk_heap(cmd), arity);
   libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, params);
   libkk_interlock_source_output_setup(cmd, kk_grid_1d(1), true, params, kk_heap(cmd));
   libkk_interlock_source_output(cmd, grid, true, params, arity);
   return (struct kk_draw_data){
      .primitive_type = arity == 1 ? MTL_PRIMITIVE_TYPE_POINT : MTL_PRIMITIVE_TYPE_LINE,
      .grid = kk_grid_indirect(capture + offsetof(struct poly_geometry_capture, draw)),
   };
}

#endif
