// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'poly-retained-raster';
export const sourceFiles = ['poly-retained-raster-types.h', 'poly-retained-raster-command.h',
  'poly-retained.h'].map(f => 'kosmickrisp-patches/' + f);

export function apply({edit, edits}) {
  for (const [from, to] of [
    ['poly-retained-raster-types.h', 'vulkan/kk_poly_retained_raster_types.h'],
    ['poly-retained-raster-command.h', 'vulkan/kk_poly_retained_raster_command.h'],
    ['poly-retained.h', 'libkk/kk_poly_retained.h'],
  ]) edits.set('src/kosmickrisp/' + to, fs.readFileSync(new URL('./' + from, import.meta.url), 'utf8'));

  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s
    .replace('#include "kk_poly_layer_replay_types.h"',
      '#include "kk_poly_layer_replay_types.h"\n#include "kk_poly_retained_raster_types.h"')
    .replace('   struct kk_poly_layer_replay_state layer_replay;',
      '   struct kk_poly_layer_replay_state layer_replay;\n   struct kk_poly_retained_state poly_retained;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('#include "kk_poly_layer_replay_nir.h"',
      '#include "kk_poly_layer_replay_nir.h"\n#include "kk_poly_retained_raster_types.h"')
    .replace('   bool force_per_vertex = kk_barycentric_force_per_vertex(',
      `   /* Compile the existing captured sibling for an explicitly enabled VRS
    * consumer. The ordinary native pair remains selected until begin(). */
   poly_layer_replay |= layer_producer != MESA_SHADER_NONE &&
      (enabled_features->pipelineFragmentShadingRate ||
       enabled_features->primitiveFragmentShadingRate ||
       enabled_features->attachmentFragmentShadingRate);
   bool force_per_vertex = kk_barycentric_force_per_vertex(`)
    .replace('static VkResult\nkk_compile_shader(', `mtl_render_pipeline_descriptor *
kk_poly_retained_pipeline_descriptor(const struct kk_shader *shader, bool conservative)
{
   return shader && shader->attachment_pipelines ?
      shader->attachment_pipelines->descriptor[conservative ? 2 : 1] : NULL;
}

void
kk_poly_retained_lower_fragment(nir_shader *nir, const struct kk_shader_info *producer,
   const struct vk_graphics_pipeline_state *state)
{
   bool effects = state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
      state->rs->line.stipple.enable ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE) ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE);
   if (producer->polygon.primitive_vertices != 1)
      nir_shader_intrinsics_pass(nir, kk_polygon_point_coord,
         nir_metadata_control_flow, effects ? (void *)1 : NULL);
   kk_line_fragment(nir, true, state);
   kk_poly_layer_lower_fs(nir);
}

static VkResult
kk_compile_shader(`));

  edit('src/kosmickrisp/vulkan/kk_poly_layer_replay_nir.h', s => s.replace(
    '   assert(position && layer && position->instr.block == layer->instr.block);',
    `   assert(position);
   if (!layer) {
      nir_builder zero = nir_builder_at(nir_after_instr(&position->instr));
      nir_store_output(&zero, nir_imm_int(&zero, 0), nir_imm_int(&zero, 0),
         .src_type = nir_type_uint32, .io_semantics.location = VARYING_SLOT_LAYER,
         .io_semantics.num_slots = 1);
      layer = nir_instr_as_intrinsic(zero.cursor.instr);
      nir->info.outputs_written |= VARYING_BIT_LAYER;
   }
   assert(position->instr.block == layer->instr.block);`));

  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('   if (cmd->state.gfx.layer_replay.bind && vs->info.poly_layer_replay) return true;',
      '   if ((cmd->state.gfx.layer_replay.bind || cmd->state.gfx.poly_retained.consume) &&\n       vs->info.poly_layer_replay) return true;')
    .replace('#include "kk_poly_layer_replay_command.h"',
      '#include "kk_poly_retained_raster_command.h"\n#include "kk_poly_layer_replay_command.h"')
    .replace('   if (gfx->layer_replay.bind) gfx->layer_replay.per_draw_address = shader_data_gpu.gpu;',
      '   if (gfx->layer_replay.bind) gfx->layer_replay.per_draw_address = shader_data_gpu.gpu;\n   if (gfx->poly_retained.consume) gfx->poly_retained.per_draw_address = shader_data_gpu.gpu;')
    .replace('(gfx->render.layered_bc || kk_polygon_expansion_active(cmd) ||',
      '(gfx->render.layered_bc || gfx->poly_retained.consume || kk_polygon_expansion_active(cmd) ||'));
  edit('src/kosmickrisp/vulkan/kk_poly_layer_replay_command.h', s => s
    .replaceAll('kk_dispatch_draw(cmd, draw);', 'kk_poly_retained_dispatch_draw(cmd, draw);')
    .replace('         mtl_set_address(cmd->argument_table, data.gpu, KK_ARGUMENT_POLY_LAYER_REPLAY);',
      '         mtl_set_address(cmd->argument_table, data.gpu, KK_ARGUMENT_POLY_LAYER_REPLAY);\n         gfx->poly_retained.layer_selector_address = data.gpu;\n         gfx->poly_retained.logical_layer = layer;')
    .replace('      kk_poly_retained_dispatch_draw(cmd, draw);\n   }',
      '      kk_poly_retained_dispatch_draw(cmd, draw);\n      if (cmd->vk.record_result != VK_SUCCESS) return;\n   }'));

  /* The same named selector entry is nullable for VRS-only captured pairs.
   * Its size/offsets and the guard binding remain unchanged. */
  for (const file of ['src/kosmickrisp/vulkan/kk_cmd_buffer.c',
                      'src/kosmickrisp/vulkan/kk_poly_layer_replay_command.h'])
    edit(file, s => s.replaceAll('dev->vk.enabled_features.multiview',
      '(dev->vk.enabled_features.multiview || dev->vk.enabled_features.pipelineFragmentShadingRate ||\n          dev->vk.enabled_features.primitiveFragmentShadingRate || dev->vk.enabled_features.attachmentFragmentShadingRate)'));

  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('#include "kk_polygon.h"', '#include "kk_polygon.h"\n#include "kk_poly_retained.h"')
    .replace(`   uint line_vertices = p->line_mode == 2 ? 2u * (uint)(p->line_width + 0.5f) : 6u;
   if (geometry_stipple && p->line_mode == 2) line_vertices = 2;
   p->draw[0] = geometry_stipple && p->line_mode != 2 ? count / 4u * 6u :
      p->line_expand ? count / 2u * line_vertices :
      count * (p->polygon_mode == 2 ? 6 : 1);`,
      `   p->draw[0] = kk_poly_retained_vertex(count, p->polygon_mode, p->line_expand,
      p->line_mode, p->line_width, geometry_stipple, false);`)
    .replace('#include "kk_conservative.cl"', `/* GPU scheduling can select an original assembled primitive or an explicit
 * batch. The inclusive prefix keeps every clipped/expanded subdivision in one
 * draw; the original linked PrimitiveID/Layer/rate remain in the packets. */
KERNEL(1)
libkk_polygon_retained_draw(global struct poly_geometry_params *geometry,
   constant struct kk_poly_retained_range *range, global uint *draw,
   uint conservative)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint first = min(range->first, p->clip_grid[0]);
   uint end = first + min(range->count, p->clip_grid[0] - first);
   global uint *counts = (global uint *)p->clip_counts;
   uint a = first ? counts[first - 1] : 0;
   uint b = end ? counts[end - 1] : 0;
   uint geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   uint start = kk_poly_retained_vertex(a, p->polygon_mode, p->line_expand,
      p->line_mode, p->line_width, geometry_stipple, conservative);
   uint stop = kk_poly_retained_vertex(b, p->polygon_mode, p->line_expand,
      p->line_mode, p->line_width, geometry_stipple, conservative);
   draw[0] = stop - start; draw[1] = 1; draw[2] = start; draw[3] = 0;
}

#include "kk_conservative.cl"`));
}
