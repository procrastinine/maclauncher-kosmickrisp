/* Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 * Adapted from pinned Mesa Honeykrisp hk_ia_update and Poly query wiring.
 */
static uint64_t
kk_pipeline_stat_addr(const struct kk_cmd_buffer *cmd,
                      VkQueryPipelineStatisticFlagBits bit)
{
   if (!(cmd->state.pipeline_statistics_flags & bit))
      return 0;
   return cmd->state.pipeline_statistics +
          cmd->state.pipeline_statistic_offsets[ffs(bit) - 1];
}

static void
kk_update_ia_statistics(struct kk_cmd_buffer *cmd,
                        struct kk_draw_command *data)
{
   const VkQueryPipelineStatisticFlags ia =
      VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
      VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
      VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT;
   if (!(cmd->state.pipeline_statistics_flags & ia))
      return;

   uint64_t vertices = kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT);
   uint64_t primitives = kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT);
   uint64_t vs = kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT);
   bool output = !kk_geometry_shader(cmd) && !cmd->state.shaders[MESA_SHADER_TESS_EVAL];
   uint64_t cp = output ? kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT) : 0;
   uint64_t ci = output ? kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT) : 0;
   if (!(vertices || primitives || vs || cp || ci))
      return;
   unsigned views = MAX2(1, util_bitcount(cmd->state.gfx.render.view_mask));
   unsigned patch = cmd->vk.dynamic_graphics_state.ts.patch_control_points;
   for (unsigned i = 0; i < data->draw_count; ++i) {
      struct kk_draw_data draw = build_draw_data(cmd, data, i);
      uint64_t args = kk_grid_is_indirect(draw.grid) ? draw.grid.addr : 0;
      if (!kk_grid_is_indirect(draw.grid)) {
         uint32_t direct[] = {draw.grid.size.x, draw.grid.size.y, 0};
         args = kk_pool_upload(cmd, direct, sizeof(direct), 4).gpu;
         if (!args) return;
      }
      if (data->indexed && data->restart) {
         libkk_increment_ia_restart(cmd, kk_grid_1d(1024), true,
            vertices, primitives, vs, cp, ci, args, draw.index.gpu.addr,
            draw.index.gpu.range / draw.index.el_size_B, data->restart_index,
            draw.index.el_size_B, data->prim, patch, views);
      } else {
         libkk_increment_ia(cmd, kk_grid_1d(1), true,
            vertices, primitives, vs, cp, ci, args, data->prim, patch, views);
      }
   }
}

static void
kk_finish_geometry_statistics(struct kk_cmd_buffer *cmd)
{
   if (!(cmd->state.pipeline_statistics_flags &
         (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
          VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
          VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
          VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT)))
      return;
   struct kk_per_draw_data *p = &cmd->state.gfx.per_draw_data;
   bool api_gs = cmd->state.shaders[MESA_SHADER_VERTEX]->info.api_geometry;
   libkk_accumulate_geometry_statistics(cmd, kk_grid_1d(1), true,
      p->geometry_statistics[PIPE_STAT_QUERY_GS_INVOCATIONS - KK_GEOMETRY_STAT_FIRST],
      p->geometry_statistics[PIPE_STAT_QUERY_GS_PRIMITIVES - KK_GEOMETRY_STAT_FIRST],
      p->geometry_statistics[PIPE_STAT_QUERY_C_INVOCATIONS - KK_GEOMETRY_STAT_FIRST],
      p->geometry_statistics[PIPE_STAT_QUERY_C_PRIMITIVES - KK_GEOMETRY_STAT_FIRST],
      api_gs ? kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT) : 0,
      api_gs ? kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT) : 0,
      kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT),
      kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT),
      cmd->interlock_heap_active ? 1 : MAX2(1, util_bitcount(cmd->state.gfx.render.view_mask)));
}
