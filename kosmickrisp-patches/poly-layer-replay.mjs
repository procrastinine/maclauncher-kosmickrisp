// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const id = 'poly-layer-replay';
export const sourceFiles = ['kosmickrisp-patches/poly-layer-replay-types.h',
  'kosmickrisp-patches/poly-layer-replay-nir.h',
  'kosmickrisp-patches/poly-layer-replay-command.h'];

export function apply({edit, edits}) {
  for (const [input, output] of [
    ['poly-layer-replay-types.h', 'kk_poly_layer_replay_types.h'],
    ['poly-layer-replay-nir.h', 'kk_poly_layer_replay_nir.h'],
    ['poly-layer-replay-command.h', 'kk_poly_layer_replay_command.h'],
  ]) edits.set('src/kosmickrisp/vulkan/' + output,
    fs.readFileSync(new URL('./' + input, import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_compile_options.h', s => s.replace(
    '   bool independent_interface;', '   bool independent_interface;\n   bool poly_layer_replay;'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
    .replace('   KK_FEAT_COUNT,', '   KK_FEAT_POLY_LAYER_REPLAY,\n   KK_FEAT_COUNT,')
    .replace('   bool polygon_native;', '   bool polygon_native;\n   bool poly_layer_replay;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s
    .replace('struct kk_graphics_state {', '#include "kk_poly_layer_replay_types.h"\n\nstruct kk_graphics_state {\n   struct kk_poly_layer_replay_state layer_replay;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('   if (feats->sparseResidencyBuffer)',
      '   if (feats->shaderOutputLayer) key |= BITFIELD_BIT(KK_FEAT_POLY_LAYER_REPLAY);\n   if (feats->sparseResidencyBuffer)')
    .replace('#include "kk_polygon_scalar_io.h"', '#include "kk_polygon_scalar_io.h"\n#include "kk_poly_layer_replay_nir.h"')
    .replace('   gather_shader_info(shader, nir, state);',
      '   gather_shader_info(shader, nir, state);\n   shader->info.poly_layer_replay = opts->poly_layer_replay;')
    .replace('   if (stage == MESA_SHADER_FRAGMENT && kk_polygon_possible(dev, state)) {',
      '   if (stage == MESA_SHADER_FRAGMENT &&\n       (kk_polygon_possible(dev, state) || opts->poly_layer_replay)) {')
    .replace('      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,',
      '      if (!opts->poly_layer_replay || (prev_stage && prev_stage->info.polygon_point))\n      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,')
    .replace('      kk_line_fragment(point_fs, true, state);',
      '      kk_line_fragment(point_fs, true, state);\n      if (opts->poly_layer_replay) kk_poly_layer_lower_fs(point_fs);')
    .replace('      if (kk_polygon_regular_possible(dev, state))\n         kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);',
      '      if (kk_polygon_regular_possible(dev, state) || opts->poly_layer_replay)\n         kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);')
    .replace('         kk_conservative_lower_fs(conservative_fs, &prev_stage->info.polygon);',
      '         kk_conservative_lower_fs(conservative_fs, &prev_stage->info.polygon);\n         if (opts->poly_layer_replay) kk_poly_layer_lower_fs(conservative_fs);')
    .replace('            nir_shader *conservative_vs = kk_conservative_vertex_shader(nir->options);',
      '            nir_shader *conservative_vs = kk_conservative_vertex_shader(nir->options);\n            if (opts->poly_layer_replay) kk_poly_layer_lower_vs(conservative_vs);')
    .replace('      bool polygon = kk_polygon_possible(dev, state) &&',
      '      bool polygon = (kk_polygon_possible(dev, state) || opts->poly_layer_replay) &&')
    .replace('         NIR_PASS(_, point_vs, kk_nir_lower_polygon_scalar_outputs);',
      '         if (opts->poly_layer_replay) kk_poly_layer_lower_vs(point_vs);\n         NIR_PASS(_, point_vs, kk_nir_lower_polygon_scalar_outputs);')
    .replace('         if (kk_polygon_regular_possible(dev, state))\n            kk_compile_msl(pdev, point_vs, state, &shader->polygon[2]);',
      '         if (kk_polygon_regular_possible(dev, state) || opts->poly_layer_replay)\n            kk_compile_msl(pdev, point_vs, state, &shader->polygon[2]);')
    .replace('      nir = rast;', `      nir = rast;
      /* Point output keeps Poly's ordinary cached GS reader and point size. */
      if (opts->poly_layer_replay && !polygon) {
         nir_shader *layer_vs = nir_shader_clone(NULL, rast);
         NIR_PASS(_, layer_vs, nir_lower_global_vars_to_local);
         kk_poly_layer_lower_vs(layer_vs);
         kk_lower_hw_vs(layer_vs, state, shader->info.gs.mode == MESA_PRIM_POINTS);
         msl_nir_lower_clip_cull_distance(layer_vs, 0);
         NIR_PASS(_, layer_vs, kk_nir_lower_descriptors, robustness, 0, NULL);
         kk_compile_msl(pdev, layer_vs, state, &shader->polygon[2]);
      }`)
    .replace('   if (result != VK_SUCCESS || !vs->info.polygon_point)',
      '   if (result != VK_SUCCESS || !(vs->info.polygon_point || vs->info.poly_layer_replay))')
    .replace('   result = kk_compile_compute_pipeline(device, &vs->polygon[0], KK_POLY_WG_SIZE,',
      '   if (vs->info.polygon_point) {\n   result = kk_compile_compute_pipeline(device, &vs->polygon[0], KK_POLY_WG_SIZE,')
    .replace('   if (vs->polygon[2].code)\n      result = kk_compile_graphics_pipeline_native',
      '   }\n   if (vs->polygon[2].code)\n      result = kk_compile_graphics_pipeline_native')
    .replace('   bool force_per_vertex = kk_barycentric_force_per_vertex(', `   bool poly_layer_replay = false;
   mesa_shader_stage layer_producer = MESA_SHADER_NONE;
   for (uint32_t i = 0; i < shader_count; ++i) {
      mesa_shader_stage stage = infos[i].stage;
      if ((stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL ||
           stage == MESA_SHADER_GEOMETRY) &&
          (layer_producer == MESA_SHADER_NONE || stage > layer_producer)) {
         layer_producer = stage;
         poly_layer_replay = infos[i].nir->info.outputs_written & VARYING_BIT_LAYER;
      }
   }
   bool force_per_vertex = kk_barycentric_force_per_vertex(`)
    .replace('emulated_stage |= (nir->xfb_info || kk_polygon_possible(dev, state))',
      'emulated_stage |= (nir->xfb_info || kk_polygon_possible(dev, state) || poly_layer_replay)')
    .replace('         if (BITSET_TEST(n->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID))',
      '         if (!poly_layer_replay && BITSET_TEST(n->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID))')
    .replace('      kk_wide_line_possible(state);',
      '      (kk_wide_line_possible(state) || poly_layer_replay);')
    .replace('   if (!explicit_geometry && (xfb || kk_polygon_possible(dev, state))) {',
      '   if (!explicit_geometry && (xfb || kk_polygon_possible(dev, state) || poly_layer_replay)) {')
    .replace('      if (kk_polygon_possible(dev, state))\n         nir_shader_intrinsics_pass(builder.shader,',
      '      if (kk_polygon_possible(dev, state) || poly_layer_replay)\n         nir_shader_intrinsics_pass(builder.shader,')
    .replace('   const struct kk_compile_options compile_options = {',
      '   const struct kk_compile_options compile_options = {\n      .poly_layer_replay = poly_layer_replay,')
    .replace('      if (stage == MESA_SHADER_FRAGMENT && info->polygon_point)',
      '      if (stage == MESA_SHADER_FRAGMENT && (info->polygon_point || info->poly_layer_replay))')
    .replaceAll('   if (shader->info.polygon_point) {',
      '   if (shader->info.polygon_point || shader->info.poly_layer_replay) {')
    .replaceAll('   if (info.polygon_point) {',
      '   if (info.polygon_point || info.poly_layer_replay) {')
    .replace('         if (info->polygon_point) {',
      '         if (info->polygon_point || info->poly_layer_replay) {'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('   if (!vs || !vs->info.polygon_point) return false;',
      '   if (!vs || !vs->info.polygon_point) return false;\n   if (cmd->state.gfx.layer_replay.bind && vs->info.poly_layer_replay) return true;')
    .replace('   if (!gs && vs && vs->info.polygon_native && !kk_polygon_expansion_active(cmd))',
      '   if (!gs && vs && vs->info.polygon_native && !kk_polygon_expansion_active(cmd) &&\n       !(cmd->state.gfx.layer_replay.bind && vs->info.poly_layer_replay))')
    .replace('kk_attachment_pipeline(cmd, vs, gfx->conservative ? 2 : gfx->polygon_points);',
      'kk_attachment_pipeline(cmd, vs, gfx->conservative ? 2 :\n            (gfx->polygon_points || (gfx->layer_replay.bind && vs->info.poly_layer_replay)));')
    .replace('   mtl_set_address(cmd->argument_table, shader_data_gpu.gpu, KK_ARGUMENT_PER_DRAW);',
      '   mtl_set_address(cmd->argument_table, shader_data_gpu.gpu, KK_ARGUMENT_PER_DRAW);\n   if (gfx->layer_replay.bind) gfx->layer_replay.per_draw_address = shader_data_gpu.gpu;')
    .replace('static void\nkk_upload_per_draw_data(',
      '#include "kk_poly_layer_replay_command.h"\n\nstatic void\nkk_upload_per_draw_data(')
    .replace('      kk_dispatch_draw(cmd, draw_data);',
      '      kk_poly_layer_dispatch_draw(cmd, draw_data);'));
  edit('src/kosmickrisp/libkk/kk_argument_table.h', s => s
    .replace('#define KK_ARGUMENT_INTERLOCK_GUARD  6u',
      '#define KK_ARGUMENT_INTERLOCK_GUARD  6u\n#define KK_ARGUMENT_POLY_LAYER_REPLAY (KK_ARGUMENT_INTERLOCK_GUARD + 1u)')
    .replace('   unsigned interlock_guard;', '   unsigned interlock_guard;\n   unsigned poly_layer_replay;')
    .replace('   return highest + 1u;', '   if (use.poly_layer_replay)\n      highest = KK_ARGUMENT_POLY_LAYER_REPLAY;\n   return highest + 1u;')
    .replace('KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_INTERLOCK_GUARD) "\\n"',
      'KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_INTERLOCK_GUARD) "\\n" \\\n   "#define KK_ARGUMENT_POLY_LAYER_REPLAY " KK_ARGUMENT_VALUE_STRING(KK_ARGUMENT_POLY_LAYER_REPLAY) "\\n"'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s
    .replace('            .interlock_guard = dev->interlock_guard != NULL,',
      '            .interlock_guard = dev->interlock_guard != NULL,\n            .poly_layer_replay = dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader,')
    .replace('      cmd->argument_table = mtl_new_argument_table(dev->mtl_handle, desc);',
      '      cmd->argument_table = mtl_new_argument_table(dev->mtl_handle, desc);\n      if (dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader)\n         mtl_set_address(cmd->argument_table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);'));
  edit('src/kosmickrisp/vulkan/kk_dgc.m', s => s.replace(
    '            .linked_graphics = !compute,',
    '            .linked_graphics = !compute,\n            .poly_layer_replay = !compute,'));
  edit('src/kosmickrisp/vulkan/kk_dgc_shader.h', s => s.replace(
    '      cmd.set_vertex_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW); cmd.set_fragment_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW);\\n',
    '      cmd.set_vertex_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW); cmd.set_fragment_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW);\\n      cmd.set_vertex_buffer((device void *)0,KK_ARGUMENT_POLY_LAYER_REPLAY); cmd.set_fragment_buffer((device void *)0,KK_ARGUMENT_POLY_LAYER_REPLAY);\\n'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('static void\nemit_inputs(', `static bool
msl_uses_poly_layer_selector(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_buffer_ptr_kk &&
                nir_intrinsic_binding(intr) == KK_ARGUMENT_POLY_LAYER_REPLAY)
               return true;
         }
      }
   }
   return false;
}

static void
emit_inputs(`)
    .replace('   msl_interlock_inputs(ctx);', `   msl_interlock_inputs(ctx);
   if (msl_uses_poly_layer_selector(shader))
      P_IND(ctx, "constant uint2 *poly_layer_selector [[buffer(%u)]],\\n",
            KK_ARGUMENT_POLY_LAYER_REPLAY);`)
    .replace('      P(ctx, "(ulong)&buf%d.contents[0];\\n", nir_intrinsic_binding(instr));', `      if (nir_intrinsic_binding(instr) == KK_ARGUMENT_POLY_LAYER_REPLAY) {
         P(ctx, "(ulong)poly_layer_selector;\\n");
      } else {
         P(ctx, "(ulong)&buf%d.contents[0];\\n", nir_intrinsic_binding(instr));
      }`));

}
