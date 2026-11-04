/* SPDX-License-Identifier: MIT */
/* All generated non-native draws use the same primitive assembly, Poly stages,
 * statistics, attachment variants and rasterization path as ordinary draws. */
bool kk_dgc_needs_poly(struct kk_cmd_buffer *cmd)
{
   const struct vk_dynamic_graphics_state *dyn =
       &cmd->vk.dynamic_graphics_state;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   bool controlled_bias =
       dyn->rs.depth_bias.enable &&
       (dyn->rs.depth_bias.exact ||
        dyn->rs.depth_bias.representation !=
            VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT);
   enum mesa_prim prim = vk_topology_to_mesa(dyn->ia.primitive_topology);
   return vs->info.dgc_replay || vs->info.vs.dynamic_vi ||
          (fs && fs->info.fs.emulates_per_vertex_inputs) ||
          cmd->state.shaders[MESA_SHADER_TESS_EVAL] ||
          kk_geometry_shader(cmd) ||
          (vs->info.polygon_point &&
           (controlled_bias ||
            dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT)) ||
          prim == MESA_PRIM_TRIANGLE_FAN || mesa_prim_has_adjacency(prim) ||
          cmd->state.gfx.xfb_enabled || cmd->state.pipeline_statistics ||
          cmd->state.gfx.primitives_query[0];
}

void kk_dgc_draw_sequence(struct kk_cmd_buffer *cmd, uint64_t root,
                          uint64_t args, uint64_t binding, bool indexed, uint32_t draw_id)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   uint32_t previous_id = gfx->indirect_draw_id;
   uint64_t previous_root = gfx->generated_root;
   uint64_t previous_floor = gfx->generated_heap_floor;
   uint64_t previous_address = gfx->descriptors.root.addr;
   struct kk_addr_range indices = {0};
   if (indexed) {
      uint64_t heap = kk_heap(cmd);
      struct kk_ptr normalized = kk_pool_alloc(cmd, 20, 4);
      struct kk_ptr floor = kk_pool_alloc(cmd, 4, 4);
      if (!normalized.gpu || !floor.gpu)
         return;
      libkk_dgc_indices(
          cmd, kk_grid_1d(1024), true, heap, args, normalized.gpu, binding,
          floor.gpu, cmd->vk.dynamic_graphics_state.ia.primitive_restart_enable,
          cmd->vk.dynamic_graphics_state.ia.primitive_restart_index);
      gfx->generated_heap_floor = floor.gpu;
      args = normalized.gpu;
      indices = kk_heap_indices(cmd);
   }
   gfx->indirect_draw_id = draw_id;
   gfx->generated_root = root;
   gfx->descriptors.root_dirty = true;
   struct kk_draw_command draw = {
       .prim = vk_topology_to_mesa(
           cmd->vk.dynamic_graphics_state.ia.primitive_topology),
       .upload_mask = build_per_draw_upload_mask(cmd),
       .index_buffer = indices,
       .index_buffer_el_size_B = indexed ? 4 : 0,
       .indirect = true,
       .indexed = indexed,
       .draw_count = 1,
       .indirect_command = {.addr = args, .stride = indexed ? 20 : 16},
   };
   kk_draw(cmd, &draw);
   gfx->indirect_draw_id = previous_id;
   gfx->generated_root = previous_root;
   gfx->generated_heap_floor = previous_floor;
   gfx->descriptors.root.addr = previous_address;
   gfx->descriptors.root_dirty = true;
}

#include "kk_dgc_layered.h"
