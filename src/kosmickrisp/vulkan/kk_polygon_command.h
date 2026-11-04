/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT */

static struct kk_draw_data
kk_launch_polygon_points(struct kk_cmd_buffer *cmd, struct kk_grid grid,
                          uint64_t geometry_params)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   struct kk_shader *gs = kk_geometry_shader(cmd);
   mtl_compute_encoder *enc = cs_get_compute(cmd);
   struct mtl_size local = {KK_POLY_WG_SIZE, 1, 1};
   if (cmd->interlock_heap_active)
      kk_interlock_source_setup(cmd, geometry_params);
   else
      libkk_polygon_capture_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));
   enc = cs_get_compute(cmd);
   mtl_barrier_after_encoder_stages(enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
   mtl_compute_set_pipeline_state(enc, vs->pipeline.gfx.polygon_capture);
   if (grid.mode == KK_GRID_INDIRECT_LOCAL)
      grid = kk_grid_indirect(gfx->polygon_capture + offsetof(struct poly_geometry_capture, dispatch_groups));
   kk_dispatch_compute(enc, grid, local);

   bool queries = cmd->state.pipeline_statistics_flags &
      (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT);
   for (unsigned i = 0; i < POLY_MAX_VERTEX_STREAMS; ++i)
      queries |= gfx->xfb_query[i] || gfx->primitives_query[i];
   if (gs->info.geometry.xfb || queries) {
      if (gs->info.geometry.prefix_sum)
         libkk_prefix_sum_geometry(cmd,
            kk_grid_1d(1024 * gs->info.geometry.count_words), true, geometry_params);
      enc = cs_get_compute(cmd);
      if (vs->info.gs_aux_mask & BITFIELD_BIT(1)) {
         mtl_barrier_after_encoder_stages(enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
         mtl_compute_set_pipeline_state(enc, vs->pipeline.gfx.gs_aux[1]);
         kk_dispatch_compute(enc, kk_grid_1d(1), (struct mtl_size){1, 1, 1});
      }
   }
   uint64_t capture = gfx->polygon_capture;
   if (vs->pipeline.gfx.polygon_xfb) {
      enc = cs_get_compute(cmd);
      mtl_barrier_after_encoder_stages(enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
      mtl_compute_set_pipeline_state(enc, vs->pipeline.gfx.polygon_xfb);
      kk_dispatch_compute(enc, kk_grid_indirect(capture +
         offsetof(struct poly_geometry_capture, capture_groups)), local);
   }

   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   if (dyn->rs.rasterizer_discard_enable ||
       (gfx->is_cull_front_and_back && vs->info.polygon.primitive_vertices > 2))
      return (struct kk_draw_data){.grid = kk_grid_1d(0)};

   if (cmd->interlock_heap_active && kk_interlock_source_arity(cmd) < 3)
      return kk_interlock_source_assemble(cmd, geometry_params);

   struct kk_grid triangles = kk_grid_indirect(capture +
      offsetof(struct poly_geometry_capture, clip_groups));
   if (gfx->conservative) {
      libkk_conservative_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));
   } else {
      libkk_polygon_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));
   }
   if (gfx->conservative)
      libkk_conservative_prepare(cmd, triangles, true, geometry_params, kk_heap(cmd));
   libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   if (fs && fs->info.fs.line_geometry_stipple && dyn->rs.line.stipple.enable &&
       (vs->info.polygon.primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE)) {
      libkk_polygon_dash_counts(cmd, kk_grid_indirect(capture +
         offsetof(struct poly_geometry_capture, clip_groups)), true, geometry_params, kk_heap(cmd));
      libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);
   }
   if (gfx->conservative) {
      libkk_conservative_output_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));
      libkk_conservative_output(cmd, triangles, true, geometry_params);
   } else {
      libkk_polygon_output_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));
      libkk_polygon_output(cmd, triangles, true, geometry_params);
   }
   return (struct kk_draw_data){
      .primitive_type = kk_polygon_raster_primitive(vs->info.polygon.primitive_vertices,
         dyn->rs.polygon_mode, dyn->rs.line.mode),
      .grid = kk_grid_indirect(capture + offsetof(struct poly_geometry_capture, draw)),
   };
}
