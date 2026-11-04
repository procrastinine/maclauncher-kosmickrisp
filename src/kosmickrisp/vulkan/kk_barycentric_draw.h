/* SPDX-License-Identifier: MIT
 * Per-vertex draw emulation adapted from Mesa !45585 (Sascha Scandella).
 */
/* Largest index buffer, in elements, an identity index buffer is made for. */
#define KK_PER_VERTEX_MAX_IDENTITY (1u << 20)

/* Indirect lists can be drawn from their own command. */
static bool
per_vertex_draws_raw_indirect(const struct kk_draw_command *data)
{
   if (data->prim != MESA_PRIM_POINTS && data->prim != MESA_PRIM_LINES &&
       data->prim != MESA_PRIM_TRIANGLES)
      return false;
   return !data->indexed ||
          data->index_buffer.range / data->index_buffer_el_size_B <=
             KK_PER_VERTEX_MAX_IDENTITY;
}

/* Index buffer 0, 1, 2, ... with at least count elements, cached for the
 * command buffer. */
static struct kk_addr_range
kk_per_vertex_identity(struct kk_cmd_buffer *cmd, uint32_t count)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   count = MAX2(count, 1u);
   if (gfx->index.identity.range < count * sizeof(uint32_t)) {
      struct kk_ptr ptr =
         kk_pool_alloc(cmd, count * sizeof(uint32_t), sizeof(uint32_t));
      if (unlikely(!ptr.gpu))
         return gfx->index.identity;
      uint32_t *indices = ptr.cpu;
      for (uint32_t i = 0u; i < count; i++)
         indices[i] = i;
      gfx->index.identity = (struct kk_addr_range){
         .addr = ptr.gpu,
         .range = count * sizeof(uint32_t),
      };
   }
   return gfx->index.identity;
}

/* Indexed draws are issued as non-indexed so every triangle corner gets its
 * own vertex shader invocation. The vertex shader reads the index buffer.
 * Indirect lists keep their command: indexed ones read an identity index
 * buffer instead of the application's. */
static void
kk_prepare_per_vertex_draw(struct kk_cmd_buffer *cmd,
                           const struct kk_draw_command *data, uint32_t draw_id,
                           uint64_t converted_draws, struct kk_draw_data *draw)
{
   struct kk_per_draw_data *per_draw = &cmd->state.gfx.per_draw_data;
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   per_draw->per_vertex_heap =
      fs->info.fs.per_vertex_uses_heap ? kk_heap(cmd) : 0u;
   per_draw->per_vertex_topology = data->prim;
   per_draw->per_vertex_indirect_addr = 0u;
   per_draw->per_vertex_raw_indirect = 0u;

   if (kk_grid_is_indirect(draw->grid)) {
      per_draw->per_vertex_indirect_addr =
         data->indirect_command.addr + draw_id * data->indirect_command.stride;
      per_draw->per_vertex_index_addr = 0u;
      per_draw->per_vertex_index_count = 0u;
      if (draw->index.el_size_B) {
         struct kk_addr_range index_range =
            kk_sanitize_index_range(cmd, draw->index.gpu);
         per_draw->per_vertex_index_addr = index_range.addr;
         per_draw->per_vertex_index_count =
            index_range.range / draw->index.el_size_B;
      }
      per_draw->per_vertex_index_size = draw->index.el_size_B;
      per_draw->per_vertex_first_vertex = 0u;
      per_draw->per_vertex_base_vertex = 0;
      if (per_vertex_draws_raw_indirect(data)) {
         per_draw->per_vertex_raw_indirect = 1u;
         if (draw->index.el_size_B) {
            struct kk_addr_range identity =
               kk_per_vertex_identity(cmd, per_draw->per_vertex_index_count);
            draw->index.gpu = identity;
            draw->index.el_size_B = 4u;
         }
         return;
      }
      draw->grid = kk_grid_indirect(converted_draws +
                                    draw_id * sizeof(VkDrawIndirectCommand));
      draw->index.el_size_B = 0u;
      draw->vertex_offset = 0u;
      draw->primitive_type = mesa_prim_to_mtl_primitive_type(
         data->prim == MESA_PRIM_TRIANGLE_FAN ||
               data->prim == MESA_PRIM_TRIANGLE_STRIP
            ? MESA_PRIM_TRIANGLES
         : data->prim == MESA_PRIM_LINE_STRIP ? MESA_PRIM_LINES
                                              : data->prim);
      return;
   }

   /* Issue an equivalent list with one vertex per primitive vertex. */
   uint32_t count = draw->grid.size.x;
   enum mesa_prim list_prim;
   uint32_t prims, prim_vertices;
   switch (data->prim) {
   case MESA_PRIM_POINTS:
      list_prim = MESA_PRIM_POINTS;
      prim_vertices = 1u;
      prims = count;
      break;
   case MESA_PRIM_LINES:
      list_prim = MESA_PRIM_LINES;
      prim_vertices = 2u;
      prims = count / 2u;
      break;
   case MESA_PRIM_LINE_STRIP:
      list_prim = MESA_PRIM_LINES;
      prim_vertices = 2u;
      prims = count >= 2u ? count - 1u : 0u;
      break;
   case MESA_PRIM_TRIANGLES:
      list_prim = MESA_PRIM_TRIANGLES;
      prim_vertices = 3u;
      prims = count / 3u;
      break;
   default:
      list_prim = MESA_PRIM_TRIANGLES;
      prim_vertices = 3u;
      prims = count >= 3u ? count - 2u : 0u;
      break;
   }
   draw->grid.size.x = prims * prim_vertices;
   draw->primitive_type = mesa_prim_to_mtl_primitive_type(list_prim);

   if (draw->index.el_size_B) {
      struct kk_addr_range index_range =
         kk_sanitize_index_range(cmd, draw->index.gpu);
      per_draw->per_vertex_index_addr = index_range.addr;
      per_draw->per_vertex_index_size = draw->index.el_size_B;
      per_draw->per_vertex_index_count =
         index_range.range / draw->index.el_size_B;
      per_draw->per_vertex_first_vertex = 0u;
      per_draw->per_vertex_base_vertex = (int32_t)draw->vertex_offset;
      draw->index.el_size_B = 0u;
      draw->vertex_offset = 0u;
   } else {
      per_draw->per_vertex_index_addr = 0u;
      per_draw->per_vertex_index_size = 0u;
      per_draw->per_vertex_index_count = 0u;
      per_draw->per_vertex_first_vertex = draw->vertex_offset;
      per_draw->per_vertex_base_vertex = 0;
      draw->vertex_offset = 0u;
   }
}

/* The PerVertexKHR emulation of a GS runs on its rasterization VS. That draw
 * is set up like an application draw of the GS output topology: restarts
 * between output strips are unrolled to lists, then each primitive corner
 * gets its own vertex shader invocation. The per-draw data is uploaded again
 * for the rasterization draw. */
static bool
kk_prepare_gs_per_vertex_draw(struct kk_cmd_buffer *cmd,
                              const struct kk_draw_command *data,
                              struct kk_draw_data *rast)
{
   struct kk_shader *gs = kk_geometry_shader(cmd);
   struct kk_draw_command gs_draw = {
      .prim = gs->info.gs.mode,
      .index_buffer = rast->index.gpu,
      .index_buffer_el_size_B = rast->index.el_size_B,
      .indexed = rast->index.el_size_B != 0u,
      .flatshade_first = data->flatshade_first,
      .draw_count = 1u,
   };
   gs_draw.restart = gs_draw.indexed && gs_draw.prim != MESA_PRIM_POINTS;
   gs_draw.restart_index =
      rast->index.el_size_B == sizeof(uint16_t) ? UINT16_MAX : UINT32_MAX;

   if (kk_grid_is_indirect(rast->grid)) {
      gs_draw.indirect = true;
      gs_draw.indirect_command.addr = rast->grid.addr;
      gs_draw.indirect_command.stride =
         gs_draw.indexed ? sizeof(VkDrawIndexedIndirectCommand)
                         : sizeof(VkDrawIndirectCommand);
   } else if (gs_draw.indexed) {
      gs_draw.indexed_draws[0] = (VkDrawIndexedIndirectCommand){
         .indexCount = rast->grid.size.x,
         .instanceCount = rast->grid.size.y,
         .vertexOffset = (int32_t)rast->vertex_offset,
         .firstInstance = rast->grid.size.z,
      };
   } else {
      gs_draw.draws[0] = (VkDrawIndirectCommand){
         .vertexCount = rast->grid.size.x,
         .instanceCount = rast->grid.size.y,
         .firstVertex = rast->vertex_offset,
         .firstInstance = rast->grid.size.z,
      };
   }

   if (gs_draw.restart && !kk_unroll_geometry(cmd, &gs_draw, false))
      return false;

   struct kk_draw_data draw = *rast;
   if (gs_draw.indirect) {
      draw.grid = kk_grid_indirect(gs_draw.indirect_command.addr);
      draw.index.gpu = gs_draw.index_buffer;
      draw.index.el_size_B = gs_draw.index_buffer_el_size_B;
      draw.vertex_offset = 0u;
   }

   uint64_t converted_draws = 0u;
   if (gs_draw.indirect && !per_vertex_draws_raw_indirect(&gs_draw)) {
      struct kk_ptr out = kk_pool_alloc(cmd, sizeof(VkDrawIndirectCommand), 4u);
      if (unlikely(!out.gpu))
         return false;
      libkk_per_vertex_draw(cmd, kk_grid_1d(1u), true, out.gpu,
                            gs_draw.indirect_command.addr,
                            gs_draw.indirect_command.stride / sizeof(uint32_t),
                            gs_draw.indexed, gs_draw.prim);
      converted_draws = out.gpu;
   }

   kk_prepare_per_vertex_draw(cmd, &gs_draw, 0u, converted_draws, &draw);

   struct kk_ptr per_draw =
      kk_pool_upload(cmd, &cmd->state.gfx.per_draw_data,
                     sizeof(cmd->state.gfx.per_draw_data), 8u);
   if (unlikely(!per_draw.gpu))
      return false;
   mtl_set_address(cmd->argument_table, per_draw.gpu, 2u);

   *rast = draw;
   return true;
}
