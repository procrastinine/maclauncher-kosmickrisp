// SPDX-License-Identifier: MIT
// Shared primitive-corner transport adapted from Mesa !45585 (Sascha Scandella).
import fs from 'node:fs';

export const sourceFiles = [
  "kosmickrisp-patches/barycentric-kk_barycentric_draw.h",
  "kosmickrisp-patches/barycentric-kk_barycentric_shader.h",
  "kosmickrisp-patches/barycentric-kk_nir_lower_per_vertex.c",
  "kosmickrisp-patches/barycentric-kk_nir_lower_per_vertex.h"
];

export function applyPerVertexIO({edit, edits}) {
  edit("src/kosmickrisp/libkk/kk_draws.cl", s => {
    s = s.replace(`   constant uint32_t *in_draw, global uint32_t *out_draw,
   uint32_t in_draw_stride_el, uint32_t restart_index,
   uint32_t index_buffer_size_el, uint32_t in_el_size_B,
   uint32_t out_el_size_B, uint32_t flatshade_first, uint32_t mode)
{
   uint gid = cl_group_id.x;
   in_draw += gid * in_draw_stride_el;
`, `   constant uint32_t *in_draw, global uint32_t *out_draw,
   uint32_t in_draw_stride_el, uint32_t restart_index,
   uint32_t index_buffer_size_el, uint32_t in_el_size_B,
   uint32_t out_el_size_B, uint32_t flatshade_first,
   uint32_t emulate_flatshade_last, uint32_t mode)
{
   uint gid = cl_group_id.x;
   in_draw += gid * in_draw_stride_el;
`);
    s = s.replace(`   POLY_DECL_UNROLL_RESTART_SCRATCH(scratch, 1024);
   poly_unroll_geometry(out_draw, heap, in_draw, index_buffer,
                        index_buffer_size_el, in_el_size_B, out_el_size_B,
                        restart_index, flatshade_first, true, mode, scratch);
}

/* Unrolls restart without reordering vertices for a GS input. */
`, `   POLY_DECL_UNROLL_RESTART_SCRATCH(scratch, 1024);
   poly_unroll_geometry(out_draw, heap, in_draw, index_buffer,
                        index_buffer_size_el, in_el_size_B, out_el_size_B,
                        restart_index, flatshade_first, emulate_flatshade_last,
                        mode, scratch);
}

/*
 * PerVertexKHR emulation issues every draw as a non-indexed point, line or
 * triangle list starting at vertex 0, with one vertex per primitive vertex.
 * The vertex shader reads the first vertex/index and vertex offset from the
 * original command.
 */
KERNEL(32)
libkk_per_vertex_draw(global uint32_t *out, constant uint32_t *in,
                      uint32_t in_stride_el, uint32_t indexed, uint32_t mode)
{
   uint draw = cl_global_id.x;
   in += draw * in_stride_el;
   out += draw * 4;

   uint32_t count = in[0];
   uint32_t prims, prim_vertices;
   switch (mode) {
   case MESA_PRIM_POINTS:
      prim_vertices = 1;
      prims = count;
      break;
   case MESA_PRIM_LINES:
      prim_vertices = 2;
      prims = count / 2;
      break;
   case MESA_PRIM_LINE_STRIP:
      prim_vertices = 2;
      prims = count >= 2 ? count - 1 : 0;
      break;
   case MESA_PRIM_TRIANGLES:
      prim_vertices = 3;
      prims = count / 3;
      break;
   default:
      prim_vertices = 3;
      prims = count >= 3 ? count - 2 : 0;
      break;
   }

   out[0] = prims * prim_vertices;
   out[1] = in[1];
   out[2] = 0;
   out[3] = indexed ? in[4] : in[3];
}

/* Unrolls restart without reordering vertices for a GS input. */
`);
    s = s.replace(`                       constant uint32_t *in_draw, global uint32_t *out_draw,
                       uint32_t in_draw_stride_el,
                       uint32_t index_buffer_size_el, uint32_t in_el_size_B,
                       uint32_t flatshade_first, uint32_t mode)
{
   uint tid = cl_local_id.x;
   in_draw += cl_group_id.x * in_draw_stride_el;
`, `                       constant uint32_t *in_draw, global uint32_t *out_draw,
                       uint32_t in_draw_stride_el,
                       uint32_t index_buffer_size_el, uint32_t in_el_size_B,
                       uint32_t flatshade_first,
                       uint32_t emulate_flatshade_last, uint32_t mode)
{
   uint tid = cl_local_id.x;
   in_draw += cl_group_id.x * in_draw_stride_el;
`);
    s = s.replace(`         uint id =
            poly_vertex_id_for_topology(mode, flatshade_first, i, vtx, prims);
         uint x = (i * out_per_prim) + poly_output_vertex_id_for_topology(
                                          out_mode, flatshade_first, true, k);
         poly_store_index(
            out_ptr, sizeof(uint32_t), x,
            poly_load_index(in_ptr, in_range_el, id, in_el_size_B));
`, `         uint id =
            poly_vertex_id_for_topology(mode, flatshade_first, i, vtx, prims);
         uint x = (i * out_per_prim) + poly_output_vertex_id_for_topology(
                                          out_mode, flatshade_first,
                                          emulate_flatshade_last, k);
         poly_store_index(
            out_ptr, sizeof(uint32_t), x,
            poly_load_index(in_ptr, in_range_el, id, in_el_size_B));
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_cmd_buffer.h", s => {
    s = s.replace(`   /* Whether the last vertex of a primitive is its provoking vertex */
   uint32_t provoking_last;
   uint32_t rasterization_stream;
};

struct kk_attachment {
`, `   /* Whether the last vertex of a primitive is its provoking vertex */
   uint32_t provoking_last;
   uint32_t rasterization_stream;

   /* PerVertexKHR emulation. Index buffer of the draw, 0 if non-indexed. */
   uint64_t per_vertex_index_addr;
   uint32_t per_vertex_index_size;
   uint32_t per_vertex_first_vertex;
   int32_t per_vertex_base_vertex;
   uint32_t per_vertex_topology;
   /* Number of index buffer elements. */
   uint32_t per_vertex_index_count;
   /* The original indirect command is drawn, so vertex IDs start at its first
    * vertex/index plus vertex offset. */
   uint32_t per_vertex_raw_indirect;
   /* Original indirect command of the draw, 0 for direct draws. */
   uint64_t per_vertex_indirect_addr;
   /* Heap the PerVertexKHR buffer is allocated from, 0 if unused. */
   uint64_t per_vertex_heap;
};

struct kk_attachment {
`);
    s = s.replace(`      struct kk_addr_range gpu;
      uint8_t bytes_per_index;
      uint64_t null_addr;
   } index;

   /* Vertex buffers */
`, `      struct kk_addr_range gpu;
      uint8_t bytes_per_index;
      uint64_t null_addr;
      /* uint32 0, 1, 2, ... for PerVertexKHR emulated indirect draws. */
      struct kk_addr_range identity;
   } index;

   /* Vertex buffers */
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_cmd_draw.c", s => {
    s = s.replace(`
/* Unrolling will always be done through indirect rendering, so if this is
 * called from non-indirect calls, we will fake it. */
static bool
kk_unroll_geometry(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;
`, `
/* Unrolling will always be done through indirect rendering, so if this is
 * called from non-indirect calls, we will fake it. */
/* With emulate_flatshade_last, last-vertex provoking triangles are rotated so
 * the provoking vertex comes first. The PerVertexKHR emulation needs the
 * Vulkan vertex order instead and handles the provoking vertex itself. */
static bool
kk_unroll_geometry(struct kk_cmd_buffer *cmd, struct kk_draw_command *data,
                   bool emulate_flatshade_last)
{
   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;
`);
    s = s.replace(`      .in_el_size_B = data->index_buffer_el_size_B,
      .out_el_size_B = 4u,
      .flatshade_first = data->flatshade_first,
      .mode = data->prim,
   };

`, `      .in_el_size_B = data->index_buffer_el_size_B,
      .out_el_size_B = 4u,
      .flatshade_first = data->flatshade_first,
      .emulate_flatshade_last = emulate_flatshade_last,
      .mode = data->prim,
   };

`);
    s = s.replace(`/* Metal has no adjacency topologies, so draws without a GS draw the lines or
 * triangles they rasterize instead. */
static bool
kk_unroll_adjacency(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;
`, `/* Metal has no adjacency topologies, so draws without a GS draw the lines or
 * triangles they rasterize instead. */
static bool
kk_unroll_adjacency(struct kk_cmd_buffer *cmd, struct kk_draw_command *data,
                    bool emulate_flatshade_last)
{
   if (unlikely(!kk_convert_to_indirect_draw(cmd, data)))
      return false;
`);
    s = s.replace(`                       : 0u,
      .in_el_size_B = data->index_buffer_el_size_B,
      .flatshade_first = data->flatshade_first,
      .mode = data->prim,
   };

`, `                       : 0u,
      .in_el_size_B = data->index_buffer_el_size_B,
      .flatshade_first = data->flatshade_first,
      .emulate_flatshade_last = emulate_flatshade_last,
      .mode = data->prim,
   };

`);
    s = s.replace(`      if (tess_args.gpu) {
         kk_upload_tess_params(cmd, tess_args.cpu, draw);
      }
   }

   if (geom) {
`, `      if (tess_args.gpu) {
         kk_upload_tess_params(cmd, tess_args.cpu, draw);
      }
      struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
      if (!geom && fs && fs->info.fs.emulates_per_vertex_inputs) {
         struct kk_device *dev = kk_cmd_buffer_device(cmd);
         struct kk_per_draw_data *pd = &gfx->per_draw_data;
         pd->per_vertex_indirect_addr = gfx->tess.out_draws_addr;
         pd->per_vertex_index_addr = dev->heap->gpu + sizeof(struct poly_heap);
         pd->per_vertex_index_size = 4u;
         pd->per_vertex_index_count =
            (dev->heap->size_B - sizeof(struct poly_heap)) / 4u;
         pd->per_vertex_first_vertex = 0u;
         pd->per_vertex_base_vertex = 0;
         pd->per_vertex_topology = gfx->tess.prim;
         pd->per_vertex_heap =
            fs->info.fs.per_vertex_uses_heap ? kk_heap(cmd) : 0u;
      }

   }

   if (geom) {
`);
    s = s.replace(`         dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   }

   struct kk_ptr shader_data_gpu =
      kk_pool_upload(cmd, &gfx->per_draw_data, sizeof(gfx->per_draw_data), 8u);
   if (unlikely(!shader_data_gpu.gpu))
      return;

`, `         dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   }

   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   bool per_vertex = fs && fs->info.fs.emulates_per_vertex_inputs;
   if (per_vertex)
      gfx->per_draw_data.provoking_last =
         cmd->vk.dynamic_graphics_state.rs.provoking_vertex ==
         VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   /* The optional tail adds no upload bytes to ordinary shaders. */
   uint32_t per_draw_size = per_vertex ? sizeof(gfx->per_draw_data) :
      offsetof(struct kk_per_draw_data, per_vertex_index_addr);
   struct kk_ptr shader_data_gpu =
      kk_pool_upload(cmd, &gfx->per_draw_data, per_draw_size, 8u);
   if (unlikely(!shader_data_gpu.gpu))
      return;

`);
    s = s.replace(`   struct kk_draw_data draw = {
      .index.gpu = data->index_buffer,
      .index.el_size_B = data->index_buffer_el_size_B,
      .primitive_type =
         emulated ? 0u : mesa_prim_to_mtl_primitive_type(data->prim),
   };

   if (data->indirect) {
`, `   struct kk_draw_data draw = {
      .index.gpu = data->index_buffer,
      .index.el_size_B = data->index_buffer_el_size_B,
      /* Fans only get here with the PerVertexKHR emulation, which draws them
       * as triangle lists. */
      .primitive_type = emulated ? 0u
                                 : mesa_prim_to_mtl_primitive_type(
                                      data->prim == MESA_PRIM_TRIANGLE_FAN
                                         ? MESA_PRIM_TRIANGLES
                                         : data->prim),
   };

   if (data->indirect) {
`);
    s = s.replace(`
#include "kk_statistics_command.h"

static void
kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
`, `
#include "kk_statistics_command.h"

#include "kk_barycentric_draw.h"

static void
kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
`);
    s = s.replace(`
   kk_update_ia_statistics(cmd, data);

   /* Unroll geometry. Skip draw if we fail. */
   if (requires_unroll(cmd, data) && !kk_unroll_geometry(cmd, data))
      return;

   if (!geom && mesa_prim_has_adjacency(data->prim) &&
       !kk_unroll_adjacency(cmd, data))
      return;

   /* Patch lists do not restart, so only a GS fed by the input assembly needs
`, `
   kk_update_ia_statistics(cmd, data);

   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   bool emulates_per_vertex = fs && fs->info.fs.emulates_per_vertex_inputs;
   bool per_vertex = !tess && !geom && emulates_per_vertex;

   /* Unroll geometry. Skip draw if we fail. The per-vertex emulation issues
    * its own lists. */
   if (!per_vertex && requires_unroll(cmd, data) &&
       !kk_unroll_geometry(cmd, data, true))
      return;

   /* Primitive restart: split into a triangle list on the GPU, keeping the
    * Vulkan vertex order, then emulate on the list. */
   if (per_vertex && data->indexed && data->restart &&
       !kk_unroll_geometry(cmd, data, false))
      return;

   /* Rotate the rewritten primitives only when flat inputs need the last
    * vertex first, like kk_unroll_geometry. The per-vertex emulation handles
    * last-vertex provoking itself and keeps the Vulkan corner order. */
   if (!geom && mesa_prim_has_adjacency(data->prim) &&
       !kk_unroll_adjacency(
          cmd, data, !per_vertex && requires_unroll_flatshade(cmd, data)))
      return;

   /* Patch lists do not restart, so only a GS fed by the input assembly needs
`);
    s = s.replace(`   if (gs_restart && !kk_convert_to_indirect_draw(cmd, data))
      return;

   for (uint32_t i = 0; i < data->draw_count; i++) {
      struct kk_draw_data draw_data = build_draw_data(cmd, data, i);

      if (data->upload_mask)
         kk_upload_per_draw_data(cmd, data->upload_mask, i, &draw_data,
                                 gs_restart);
`, `   if (gs_restart && !kk_convert_to_indirect_draw(cmd, data))
      return;

   uint64_t per_vertex_draws = 0u;
   if (per_vertex && data->indirect && !per_vertex_draws_raw_indirect(data)) {
      struct kk_ptr out = kk_pool_alloc(
         cmd, data->draw_count * sizeof(VkDrawIndirectCommand), 4u);
      if (unlikely(!out.gpu))
         return;
      libkk_per_vertex_draw(cmd, kk_grid_1d(data->draw_count), true, out.gpu,
                            data->indirect_command.addr,
                            data->indirect_command.stride / sizeof(uint32_t),
                            data->indexed, data->prim);
      per_vertex_draws = out.gpu;
   }
   bool per_vertex_tess = tess && !geom && emulates_per_vertex;
   bool per_vertex_gs = geom && emulates_per_vertex;

   for (uint32_t i = 0; i < data->draw_count; i++) {
      struct kk_draw_data draw_data = build_draw_data(cmd, data, i);

      if (per_vertex)
         kk_prepare_per_vertex_draw(cmd, data, i, per_vertex_draws, &draw_data);

      if (data->upload_mask)
         kk_upload_per_draw_data(cmd, data->upload_mask, i, &draw_data,
                                 gs_restart);
`);
    s = s.replace(`         draw_data = kk_launch_gs_prerast(cmd, data, draw_data, gs_restart);
         kk_finish_geometry_statistics(cmd);
      }

      if (!geom && gfx->primitives_query[0]) {
         enum mesa_prim mode = tess ? gfx->tess.prim : data->prim;
`, `         draw_data = kk_launch_gs_prerast(cmd, data, draw_data, gs_restart);
         kk_finish_geometry_statistics(cmd);
      }
      if (per_vertex_gs &&
          !kk_prepare_gs_per_vertex_draw(cmd, data, &draw_data))
         continue;

      if (per_vertex_tess && kk_grid_is_indirect(draw_data.grid)) {
         struct kk_ptr out =
            kk_pool_alloc(cmd, sizeof(VkDrawIndirectCommand), 4u);
         if (unlikely(!out.gpu))
            continue;
         libkk_per_vertex_draw(
            cmd, kk_grid_1d(1u), true, out.gpu, draw_data.grid.addr,
            sizeof(VkDrawIndexedIndirectCommand) / sizeof(uint32_t), 1u,
            cmd->state.gfx.tess.prim);
         draw_data.grid = kk_grid_indirect(out.gpu);
         draw_data.index.el_size_B = 0u;
      }


      if (!geom && gfx->primitives_query[0]) {
         enum mesa_prim mode = tess ? gfx->tess.prim : data->prim;
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_debug.c", s => {
    s = s.replace(`   {"nir", KK_DEBUG_NIR},
   {"msl", KK_DEBUG_MSL},
   {"force_robustness", KK_DEBUG_FORCE_ROBUSTNESS},
   {NULL, 0},
};

`, `   {"nir", KK_DEBUG_NIR},
   {"msl", KK_DEBUG_MSL},
   {"force_robustness", KK_DEBUG_FORCE_ROBUSTNESS},
   {"force_per_vertex_buffer", KK_DEBUG_FORCE_PER_VERTEX_BUFFER},
   {NULL, 0},
};

`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_debug.h", s => {
    s = s.replace(`   KK_DEBUG_MSL = 1ull << 1,
   /* Forces robustness for all shaders */
   KK_DEBUG_FORCE_ROBUSTNESS = 1ull << 2,
};

extern enum kk_debug kk_mesa_debug_flags;
`, `   KK_DEBUG_MSL = 1ull << 1,
   /* Forces robustness for all shaders */
   KK_DEBUG_FORCE_ROBUSTNESS = 1ull << 2,
   /* Passes the PerVertexKHR values of emulated shaders through memory, as
    * without enough varying slots */
   KK_DEBUG_FORCE_PER_VERTEX_BUFFER = 1ull << 3,
};

extern enum kk_debug kk_mesa_debug_flags;
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_shader.c", s => {
    s = s.replace(`#include "kk_descriptor_set_layout.h"
#include "kk_device.h"
#include "kk_format.h"
#include "kk_nir_lower_vbo.h"
#include "kk_physical_device.h"
#include "kk_sampler.h"
`, `#include "kk_descriptor_set_layout.h"
#include "kk_device.h"
#include "kk_format.h"
#include "kk_nir_lower_per_vertex.h"
#include "kk_nir_lower_vbo.h"
#include "kk_physical_device.h"
#include "kk_sampler.h"
`);
    s = s.replace(`struct kk_vs_key {
   bool is_points;
   bool dynamic_vi;
   struct vk_vertex_input_state vi;
};

`, `#include "kk_barycentric_shader.h"

struct kk_vs_key {
   bool is_points;
   bool dynamic_vi;
   bool strip_fan_or_adjacency;
   bool provoking_last;
   struct vk_vertex_input_state vi;
};

`);
    s = s.replace(`   memset(key, 0, sizeof(*key));
   key->is_points = state->ia && state->ia->primitive_topology ==
                                    VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   key->dynamic_vi = !kk_vi_is_static(state);
   if (!key->dynamic_vi)
      key->vi = *state->vi;
}

`, `   memset(key, 0, sizeof(*key));
   key->is_points = state->ia && state->ia->primitive_topology ==
                                    VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
   key->dynamic_vi = !kk_vi_is_static(state);
   key->strip_fan_or_adjacency = kk_state_strip_fan_or_adjacency(state);
   key->provoking_last = kk_state_provoking_last(state);
   if (!key->dynamic_vi)
      key->vi = *state->vi;
}

`);
    s = s.replace(`   /* Check https://github.com/KhronosGroup/Vulkan-Portability/issues/54 for
    * explanation on why we need this. */
   else if (nir->info.fs.needs_full_quad_helper_invocations ||
            nir->info.fs.needs_coarse_quad_helper_invocations) {
      struct kk_physical_device *pdev = kk_device_physical(dev);

      /* Metal by default merges triangles which results in incorrect edges, see
`, `   /* Check https://github.com/KhronosGroup/Vulkan-Portability/issues/54 for
    * explanation on why we need this. */
   else if (nir->info.fs.needs_full_quad_helper_invocations ||
            nir->info.fs.needs_coarse_quad_helper_invocations ||
            (dev->vk.enabled_features.fragmentShaderBarycentric &&
             fs_needs_unmerged_triangles(nir))) {
      struct kk_physical_device *pdev = kk_device_physical(dev);

      /* Metal by default merges triangles which results in incorrect edges, see
`);
    s = s.replace(`                  struct kk_shader *prev_stage,
                  const struct vk_pipeline_robustness_state *robustness,
                  const struct vk_graphics_pipeline_state *state,
                  const VkAllocationCallbacks *pAllocator,
                  struct kk_shader **shader_out)
{
`, `                  struct kk_shader *prev_stage,
                  const struct vk_pipeline_robustness_state *robustness,
                  const struct vk_graphics_pipeline_state *state,
                  const struct kk_per_vertex_map *per_vertex_map,
                  const VkAllocationCallbacks *pAllocator,
                  struct kk_shader **shader_out)
{
`);
    s = s.replace(`   }

   gather_shader_info(shader, nir, state);
   if (stage == MESA_SHADER_FRAGMENT && kk_polygon_possible(dev, state)) {
      shader->info.fs.noperspective_outputs = kk_polygon_noperspective_outputs(nir);
      nir_shader *point_fs = nir_shader_clone(NULL, nir);
`, `   }

   gather_shader_info(shader, nir, state);
   kk_barycentric_shader_info(shader, per_vertex_map);
   if (stage == MESA_SHADER_FRAGMENT && kk_polygon_possible(dev, state)) {
      shader->info.fs.noperspective_outputs = kk_polygon_noperspective_outputs(nir);
      nir_shader *point_fs = nir_shader_clone(NULL, nir);
`);
    s = s.replace(`         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
         NIR_PASS(_, nir, poly_nir_lower_sw_vs);
      } else
         /* Metal's instance_id contains base_instance. When the emulation path
          * is taken, since we launch compute, they correctly get translated.
          * For the non-emulated path we need to subtract base_instance... */
         NIR_PASS(_, nir, msl_nir_lower_instance_id);
   } else if (stage == MESA_SHADER_TESS_CTRL) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
       * 32-bit, which we support. Lower TCS inputs & outputs to scalar. */
`, `         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
         NIR_PASS(_, nir, poly_nir_lower_sw_vs);
      } else {
         /* Metal's instance_id contains base_instance. When the emulation path
          * is taken, since we launch compute, they correctly get translated.
          * For the non-emulated path we need to subtract base_instance... */
         NIR_PASS(_, nir, msl_nir_lower_instance_id);

         bool progress = false;
         NIR_PASS(progress, nir, kk_nir_lower_vs_per_vertex_outputs,
                  per_vertex_map);
         if (progress)
            shader->info.uses_per_draw_data = true;
      }
   } else if (stage == MESA_SHADER_TESS_CTRL) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
       * 32-bit, which we support. Lower TCS inputs & outputs to scalar. */
`);
    s = s.replace(`         nir->info.stage = MESA_SHADER_COMPUTE;
         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
      }
   } else if (stage == MESA_SHADER_GEOMETRY) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
`, `         nir->info.stage = MESA_SHADER_COMPUTE;
         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
      } else {
         bool progress = false;
         NIR_PASS(progress, nir, kk_nir_lower_vs_per_vertex_outputs,
                  per_vertex_map);
         if (progress)
            shader->info.uses_per_draw_data = true;
      }
   } else if (stage == MESA_SHADER_GEOMETRY) {
      /* TODO_KOSMICKRISP: Poly lowering breaks with vectorized I/O less than
`);
    s = s.replace(`
      kk_lower_hw_vs(nir, state, shader->info.gs.mode == MESA_PRIM_POINTS);
      msl_nir_lower_clip_cull_distance(nir, 0);
      /* Descriptors were lowered with the GS, this lowers the system values
       * the hardware vertex lowering added */
      NIR_PASS(_, nir, kk_nir_lower_descriptors, robustness, 0, NULL);
`, `
      kk_lower_hw_vs(nir, state, shader->info.gs.mode == MESA_PRIM_POINTS);
      msl_nir_lower_clip_cull_distance(nir, 0);

      bool progress = false;
      NIR_PASS(progress, nir, kk_nir_lower_vs_per_vertex_outputs,
               per_vertex_map);
      if (progress)
         shader->info.uses_per_draw_data = true;
      /* Descriptors were lowered with the GS, this lowers the system values
       * the hardware vertex lowering added */
      NIR_PASS(_, nir, kk_nir_lower_descriptors, robustness, 0, NULL);
`);
    s = s.replace(`   struct kk_shader *shader = NULL;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   VkResult result =
      kk_compile_shader(dev, nir, NULL, &rs_none, NULL, alloc, &shader);
   if (result != VK_SUCCESS)
      return result;

`, `   struct kk_shader *shader = NULL;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   VkResult result =
      kk_compile_shader(dev, nir, NULL, &rs_none, NULL, NULL, alloc, &shader);
   if (result != VK_SUCCESS)
      return result;

`);
    s = s.replace(`   nir_shader *nir_shaders[shader_count + 2u];
   struct kk_shader *shaders[shader_count + 2u];

   /* Lower shaders, notably lowering IO. This is a prerequisite for intershader
    * optimization. */
   const struct vk_pipeline_robustness_state *vertex_robustness = &rs_none;
`, `   nir_shader *nir_shaders[shader_count + 2u];
   struct kk_shader *shaders[shader_count + 2u];

   bool force_per_vertex = kk_barycentric_force_per_vertex(
      shader_count, infos, state, enabled_features);

   /* Lower shaders, notably lowering IO. This is a prerequisite for intershader
    * optimization. */
   const struct vk_pipeline_robustness_state *vertex_robustness = &rs_none;
`);
    s = s.replace(`   nir_opt_varyings_bulk(nir_shaders, total_shaders, true, UINT32_MAX,
                         UINT32_MAX, nir_opts, NULL);

   for (uint32_t i = 0; i < total_shaders; i++) {
      struct kk_shader *prev_stage = i > 0 ? shaders[i - 1] : NULL;
      result =
         kk_compile_shader(dev, nir_shaders[i], prev_stage, vertex_robustness,
                           state, pAllocator, &shaders[i]);

      if (result != VK_SUCCESS) {
         /* Clean up all the shaders before this point */
`, `   nir_opt_varyings_bulk(nir_shaders, total_shaders, true, UINT32_MAX,
                         UINT32_MAX, nir_opts, NULL);

   struct kk_per_vertex_map per_vertex_map = {0};
   if (state && total_shaders >= 2u &&
       nir_shaders[total_shaders - 1u]->info.stage == MESA_SHADER_FRAGMENT &&
       (nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_VERTEX ||
        nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_TESS_EVAL ||
        nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_GEOMETRY))
      NIR_PASS(
         _, nir_shaders[total_shaders - 1u], kk_nir_lower_fs_per_vertex_inputs,
         nir_shaders[total_shaders - 2u], force_per_vertex,
         &per_vertex_map);

   if (per_vertex_map.out_of_slots) {
      for (uint32_t i = 0; i < total_shaders; i++)
         ralloc_free(nir_shaders[i]);
      memset(shaders_out, 0, shader_count * sizeof(*shaders_out));
      return vk_errorf(dev, VK_ERROR_UNKNOWN,
                       "Out of varying slots for barycentric emulation");
   }

   for (uint32_t i = 0; i < total_shaders; i++) {
      struct kk_shader *prev_stage = i > 0 ? shaders[i - 1] : NULL;
      result =
         kk_compile_shader(dev, nir_shaders[i], prev_stage, vertex_robustness,
                           state, &per_vertex_map, pAllocator, &shaders[i]);

      if (result != VK_SUCCESS) {
         /* Clean up all the shaders before this point */
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/kk_shader.h", s => {
    s = s.replace(`
      struct {
         bool uses_flat_varyings;
         uint64_t flat_outputs;
         uint64_t noperspective_outputs;
      } fs;
`, `
      struct {
         bool uses_flat_varyings;
         bool emulates_per_vertex_inputs;
         bool per_vertex_uses_heap;
         uint64_t flat_outputs;
         uint64_t noperspective_outputs;
      } fs;
`);
    return s;
  });
   edit("src/kosmickrisp/vulkan/meson.build", s => {
    s = s.replace(`  'kk_nir_lower_multiview.c',
  'kk_nir_lower_null_images.c',
  'kk_nir_lower_textures.c',
  'kk_nir_lower_vbo.h',
  'kk_nir_lower_vbo.c',
  'kk_physical_device.h',
`, `  'kk_nir_lower_multiview.c',
  'kk_nir_lower_null_images.c',
  'kk_nir_lower_textures.c',
  'kk_nir_lower_per_vertex.c',
  'kk_nir_lower_per_vertex.h',
  'kk_nir_lower_vbo.h',
  'kk_nir_lower_vbo.c',
  'kk_physical_device.h',
`);
    return s;
  });
   edits.set("src/kosmickrisp/vulkan/kk_barycentric_draw.h", fs.readFileSync(new URL('./barycentric-kk_barycentric_draw.h', import.meta.url), 'utf8'));
  edits.set("src/kosmickrisp/vulkan/kk_barycentric_shader.h", fs.readFileSync(new URL('./barycentric-kk_barycentric_shader.h', import.meta.url), 'utf8'));
  edits.set("src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c", fs.readFileSync(new URL('./barycentric-kk_nir_lower_per_vertex.c', import.meta.url), 'utf8'));
  edits.set("src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.h", fs.readFileSync(new URL('./barycentric-kk_nir_lower_per_vertex.h', import.meta.url), 'utf8'));
}
