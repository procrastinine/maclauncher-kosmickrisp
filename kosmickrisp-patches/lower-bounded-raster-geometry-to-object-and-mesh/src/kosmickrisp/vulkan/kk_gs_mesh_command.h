/* SPDX-License-Identifier: MIT */
#ifndef KK_GS_MESH_COMMAND_H
#define KK_GS_MESH_COMMAND_H
#include "kosmickrisp/libkk/kk_gs_mesh.h"

/* Called only for an actual application GS with a cached alternative. State
 * which may change after pipeline binding is deliberately checked here. */
static bool
kk_draw_gs_mesh(struct kk_cmd_buffer *cmd, struct kk_shader *vs,
                const struct kk_draw_command *command,
                const struct kk_draw_data *draw, uint32_t draw_id)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;
   if (command->restart || mesa_prim_has_adjacency(command->prim) ||
       cmd->state.shaders[MESA_SHADER_TESS_EVAL] || gfx->xfb_enabled ||
       gfx->geometry_query_mask || cmd->state.pipeline_statistics_flags ||
       dyn->rs.rasterization_stream || gfx->polygon_points ||
       gfx->conservative || gfx->layer_replay.bind ||
       gfx->poly_retained.consume || gfx->mesh_retained_consumer.consume ||
       cmd->interlock_heap_active || gfx->render.layered_bc ||
       gfx->render.view_mask || gfx->dgc_icb || kk_grid_is_indirect(draw->grid))
      return false;
   for (unsigned i = 0; i < POLY_MAX_VERTEX_STREAMS; i++)
      if (gfx->xfb_query[i] || gfx->primitives_query[i])
         return false;
   struct kk_shader *mesh = vs->gs_mesh;
   uint32_t groups =
      u_decomposed_prims_for_vertices(command->prim, draw->grid.size.x);
   if (!groups || !draw->grid.size.y)
      return true;
   if (groups > KK_MESH_MAX_DIMENSION ||
       draw->grid.size.y > KK_MESH_MAX_DIMENSION ||
       (uint64_t)groups * draw->grid.size.y > mesh->mesh->info.gs_max_objects)
      return false;
   VkResult result = kk_shader_ensure_polygon(kk_cmd_buffer_device(cmd), mesh);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return true;
   }
   mtl_render_pipeline_state *pipeline =
      kk_attachment_pipeline(cmd, mesh, KK_PIPELINE_MAIN);
   if (!pipeline)
      return true;
   struct kk_gs_mesh_draw args = {
      .index = draw->index.gpu.addr,
      .index_count =
         draw->index.el_size_B
            ? MIN2(draw->index.gpu.range / draw->index.el_size_B, UINT32_MAX)
            : 0,
      .index_size = draw->index.el_size_B,
      .first_vertex = draw->index.el_size_B ? 0 : draw->vertex_offset,
      .base_vertex = draw->vertex_offset,
      .first_instance = draw->grid.size.z,
   };
   struct kk_ptr input = kk_pool_upload(cmd, &args, sizeof(args), 8);
   if (!input.gpu)
      return true;

   struct kk_per_draw_data saved = gfx->per_draw_data;
   gfx->per_draw_data.vertex_params = input.gpu;
   gfx->per_draw_data.draw_id = draw_id;
   gfx->per_draw_data.provoking_last = !command->flatshade_first;
   struct kk_ptr data = kk_pool_upload(
      cmd, &gfx->per_draw_data,
      offsetof(struct kk_per_draw_data, multiview_index) + sizeof(uint64_t), 8);
   gfx->per_draw_data = saved;
   if (!data.gpu)
      return true;
   mtl_set_address(cmd->argument_table, data.gpu, KK_ARGUMENT_PER_DRAW);
   mtl_render_encoder *enc = cs_get_render(cmd);
   mtl_render_set_pipeline_state(enc, pipeline);
   mtl_render_set_argument_table(enc, cmd->argument_table,
                                 MTL_RENDER_STAGE_OBJECT |
                                    MTL_RENDER_STAGE_MESH |
                                    MTL_RENDER_STAGE_FRAGMENT);
   cmd->writable_msaa_render_writes |= cmd->writable_msaa_draw_writes;
   mtl_draw_mesh_threadgroups(
      enc, (struct mtl_size){groups, draw->grid.size.y, 1},
      (struct mtl_size){1, 1, 1}, (struct mtl_size){1, 1, 1});
   /* The current logical executable and all following compute/retained draws
    * keep their original pipeline and argument-table ownership. */
   mtl_render_pipeline_state *original =
      kk_attachment_pipeline(cmd, vs, KK_PIPELINE_MAIN);
   if (original)
      mtl_render_set_pipeline_state(enc, original);
   mtl_render_set_argument_table(enc, cmd->argument_table,
                                 MTL_RENDER_STAGE_VERTEX |
                                    MTL_RENDER_STAGE_FRAGMENT);
   return true;
}
#endif
