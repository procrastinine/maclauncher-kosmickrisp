// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('      /* TODO_KOSMICKRISP Remove this once unroll, tess and any compute does not',
      `      if (!geom && gfx->primitives_query[0]) {
         enum mesa_prim mode = tess ? gfx->tess.prim : data->prim;
         uint64_t draw = kk_grid_is_indirect(draw_data.grid)
            ? draw_data.grid.addr : 0;
         if (!tess && data->restart && draw_data.index.el_size_B) {
            if (!draw) {
               VkDrawIndexedIndirectCommand direct = {
                  .indexCount = draw_data.grid.size.x,
                  .instanceCount = draw_data.grid.size.y,
               };
               draw = kk_pool_upload(cmd, &direct, sizeof(direct), 4).gpu;
            }
            libkk_primitives_generated_restart(cmd, kk_grid_1d(1024), true,
               gfx->primitives_query[0], draw, draw_data.index.gpu.addr,
               draw_data.index.gpu.range / draw_data.index.el_size_B,
               draw_data.index.el_size_B, data->restart_index, mode);
         } else {
            libkk_primitives_generated(cmd, kk_grid_1d(1), true,
               gfx->primitives_query[0], draw,
               draw_data.grid.size.x, draw_data.grid.size.y, mode);
         }
      }

      /* TODO_KOSMICKRISP Remove this once unroll, tess and any compute does not`));
  edit('src/kosmickrisp/libkk/kk_geometry.cl', s => s + `
/* Count the actual post-assembly draw when no application/synthetic GS runs.
 * The same two leading words describe direct/indexed/tessellated draws.
 * Predicates and topology rewrites have already been applied. */
KERNEL(1)
libkk_primitives_generated(global uint64_t *report, constant uint32_t *draw,
                           uint32_t vertices, uint32_t instances, uint32_t mode)
{
   if (draw) {
      vertices = draw[0];
      instances = draw[1];
   }
   *report += (uint64_t)u_decomposed_prims_for_vertices(mode, vertices) * instances;
}
`);
  edit('src/kosmickrisp/libkk/kk_draws.cl', s => s + `
/* Count restart-separated primitives without rewriting the rendered indices. */
KERNEL(1024)
libkk_primitives_generated_restart(global uint64_t *report,
   constant uint32_t *draw, uint64_t indices, uint32_t range_el,
   uint32_t index_size, uint32_t restart_index, uint32_t mode)
{
   POLY_DECL_UNROLL_RESTART_SCRATCH(scratch, 1024);
   uint prims = poly_count_restart_prims(draw, indices, range_el,
                                        index_size, restart_index, mode, scratch);
   if (cl_local_id.x == 0)
      *report += (uint64_t)prims * draw[1];
}
`);
}
