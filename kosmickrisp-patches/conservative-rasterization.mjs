// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'conservative-rasterization';
export const sourceFiles = ['conservative-types.h', 'conservative.cl', 'conservative-nir.h', 'vertex-triplet.h', 'conservative-fs-link.h', 'conservative-fs-prolog.c'].map(x => 'kosmickrisp-patches/' + x);
export function apply({edit, edits}) {
  const owned = name => fs.readFileSync(new URL('./' + name, import.meta.url), 'utf8');
  edits.set('src/kosmickrisp/libkk/kk_conservative.h', owned('conservative-types.h'));
  edits.set('src/kosmickrisp/libkk/kk_conservative.cl', owned('conservative.cl'));
  edits.set('src/kosmickrisp/vulkan/kk_conservative_nir.h', owned('conservative-nir.h'));
  edits.set('src/kosmickrisp/vulkan/kk_nir_vertex_triplet.h', owned('vertex-triplet.h'));
  edits.set('src/kosmickrisp/vulkan/kk_conservative_fs_link.h', owned('conservative-fs-link.h'));
  edits.set('src/kosmickrisp/vulkan/kk_conservative_fs_prolog.c', owned('conservative-fs-prolog.c'));
  edit('src/kosmickrisp/vulkan/meson.build', s => s.replace("  'kk_shader.c',", "  'kk_shader.c',\n  'kk_conservative_fs_prolog.c',"));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c', s => s
    .replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kk_nir_vertex_triplet.h"')
    .replace(`      nir_def *value = nir_build_load_global(
         b, intr->def.num_components, 32,
         nir_iadd_imm(b, addr, buffer_offset(map, location, v) + offset),
         .align_mul = 4u);
      values[v] = intr->def.bit_size == 32 ? value :
         nir_u2uN(b, value, intr->def.bit_size);`,
      `      values[v] = kk_nir_load_primitive_corner(b, addr,
         buffer_offset(map, location, v) + offset,
         intr->def.num_components, intr->def.bit_size);`));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s + '\n#include "kk_conservative.cl"\n');
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('kk_polygon_possible(const struct kk_device *dev,',
      'kk_polygon_regular_possible(const struct kk_device *dev,')
    .replace('static uint64_t\nkk_polygon_noperspective_outputs',
      `static bool
kk_polygon_possible(const struct kk_device *dev,
                    const struct vk_graphics_pipeline_state *state)
{
   return kk_polygon_regular_possible(dev, state) || kk_conservative_possible(state);
}

static uint64_t
kk_polygon_noperspective_outputs`));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
    .replace('   KK_FEAT_COUNT,', '   KK_FEAT_CONSERVATIVE_RASTER,\n   KK_FEAT_COUNT,')
    .replace('   bool polygon_point;', '   bool polygon_point;\n   bool conservative;')
    .replace('   bool conservative;', '   bool conservative;\n   uint64_t conservative_canonical_outputs;')
    .replace('mtl_render_pipeline_state *polygon_render;', 'mtl_render_pipeline_state *polygon_render;\n         mtl_render_pipeline_state *conservative_render;')
    .replace('struct msl_compile_data polygon[4];', 'struct msl_compile_data polygon[6];')
    .replace('struct kk_shader *shader, bool polygon);', 'struct kk_shader *shader, unsigned polygon);'));
  edit('src/kosmickrisp/vulkan/kk_compile_options.h', s => s.replace(
    '   const struct kk_per_vertex_map *per_vertex_map;',
    '   const struct kk_per_vertex_map *per_vertex_map;\n   struct nir_shader *conservative_fs;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('#include "kk_barycentric_shader.h"', '#include "kk_barycentric_shader.h"\n\nstatic bool kk_conservative_possible(const struct vk_graphics_pipeline_state *state);');
    s = s.replace('   _mesa_blake3_update(&blake3_ctx, &feature_key, sizeof(feature_key));', `   _mesa_blake3_update(&blake3_ctx, &feature_key, sizeof(feature_key));
   if (kk_conservative_possible(state)) {
      uint32_t conservative = BITFIELD_BIT(KK_FEAT_CONSERVATIVE_RASTER);
      _mesa_blake3_update(&blake3_ctx, &conservative, sizeof(conservative));
   }`);
    s = s.replace('#include "kk_polygon_nir.h"', '#include "kk_polygon_nir.h"\n#include "kk_conservative_nir.h"');
    s = s.replace('   const struct kk_per_vertex_map *per_vertex_map = opts->per_vertex_map;', '   const struct kk_per_vertex_map *per_vertex_map = opts->per_vertex_map;\n   nir_shader *conservative_fs = nir->info.stage == MESA_SHADER_FRAGMENT ? opts->conservative_fs : NULL;');
    s = s.replace('   gather_shader_info(shader, nir, state);', '   shader->info.conservative = kk_conservative_possible(state);\n   gather_shader_info(shader, nir, state);');
    s = s.replace('      kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);', `      if (kk_polygon_regular_possible(dev, state))
         kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);
      else
         ralloc_free(point_fs);
      if (conservative_fs) {
         kk_conservative_lower_fs(conservative_fs, &prev_stage->info.polygon);
         kk_compile_msl(pdev, conservative_fs, state, &shader->polygon[5]);
      }`);
    s = s.replace('         kk_compile_msl(pdev, point_vs, state, &shader->polygon[2]);', `         if (kk_polygon_regular_possible(dev, state))
            kk_compile_msl(pdev, point_vs, state, &shader->polygon[2]);
         else
            ralloc_free(point_vs);
         if (shader->info.conservative) {
            nir_shader *conservative_vs = kk_conservative_vertex_shader(nir->options);
            NIR_PASS(_, conservative_vs, msl_nir_vs_io_types);
            NIR_PASS(_, conservative_vs, kk_nir_lower_descriptors, robustness, 0, NULL);
            kk_compile_msl(pdev, conservative_vs, state, &shader->polygon[4]);
         }`);
    s = s.replace('   struct kk_per_vertex_map per_vertex_map = {0};', `   nir_shader *conservative_fs = kk_conservative_possible(state) &&
      nir_shaders[total_shaders - 1u]->info.stage == MESA_SHADER_FRAGMENT
      ? nir_shader_clone(NULL, nir_shaders[total_shaders - 1u]) : NULL;
   struct kk_per_vertex_map per_vertex_map = {0};`);
    s = s.replace('      .per_vertex_map = &per_vertex_map,', '      .per_vertex_map = &per_vertex_map,\n      .conservative_fs = conservative_fs,');
    s = s.replace('         kk_msl_copy(&vs->polygon[3], &s->polygon[3]);', `         kk_msl_copy(&vs->polygon[3], &s->polygon[3]);
      if (stage == MESA_SHADER_FRAGMENT && info->conservative)
         kk_msl_copy(&vs->polygon[5], &s->polygon[5]);`);
    s = s.replace('         info->polygon_point = s->info.polygon_point;', '         info->polygon_point = s->info.polygon_point;\n         info->conservative = s->info.conservative;\n         if (info->conservative) kk_msl_copy(&vs->polygon[4], &s->polygon[4]);');
    s = s.replace('kk_compile_graphics_pipeline_native(struct kk_device *device, struct kk_shader *vs, bool polygon)', 'kk_compile_graphics_pipeline_native(struct kk_device *device, struct kk_shader *vs, unsigned polygon)');
    s = s.replace('polygon ? &vs->polygon[2] :', 'polygon ? &vs->polygon[2 * polygon] :');
    s = s.replace('polygon ? &vs->polygon[3] :', 'polygon ? &vs->polygon[2 * polygon + 1] :');
    s = s.replace('polygon ? MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED : vs->info.vs.topology', 'polygon == 2 ? MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE :\n         polygon ? MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED : vs->info.vs.topology');
    s = s.replace('   if (polygon) pipe->gfx.polygon_render = render;', '   if (polygon == 2) pipe->gfx.conservative_render = render;\n   else if (polygon) pipe->gfx.polygon_render = render;');
    s = s.replace('   result = kk_compile_graphics_pipeline_native(device, vs, true);', `   if (vs->polygon[2].code)
      result = kk_compile_graphics_pipeline_native(device, vs, true);
   if (result == VK_SUCCESS && vs->info.conservative)
      result = kk_compile_graphics_pipeline_native(device, vs, 2);`);
    s = s.replace('      mtl_release(pipe->gfx.polygon_render);', '      mtl_release(pipe->gfx.polygon_render);\n      mtl_release(pipe->gfx.conservative_render);');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_unused_attachments.h', s => s
    .replace('   bool polygon;', '   unsigned polygon;')
    .replace('struct kk_shader *shader, bool polygon,', 'struct kk_shader *shader, unsigned polygon,')
    .replace('descriptor[2];', 'descriptor[3];')
    .replace('                       bool polygon)', '                       unsigned polygon)')
    .replace('mtl_render_pipeline_state *original = polygon ?', 'mtl_render_pipeline_state *original = polygon == 2 ? shader->pipeline.gfx.conservative_render\n      : polygon ?'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    '   bool polygon_points;', '   bool polygon_points;\n   bool conservative;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
    s = s.replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kk_conservative.h"');
    s = s.replace('kk_attachment_pipeline(cmd, vs, gfx->polygon_points);', 'kk_attachment_pipeline(cmd, vs, gfx->conservative ? 2 : gfx->polygon_points);');
    s = s.replace('   bool emulates_per_vertex = fs && fs->info.fs.emulates_per_vertex_inputs;',
      '   bool emulates_per_vertex = fs && fs->info.fs.emulates_per_vertex_inputs &&\n      !gfx->conservative;');
    s = s.replace('      params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;', `      if (gfx->conservative) {
         struct kk_conservative_params conservative = {
            .polygon = point,
            .extra_size = dyn->rs.extra_primitive_overestimation_size,
            .samples = MAX2(dyn->ms.rasterization_samples, 1),
            .sample_mask = dyn->ms.sample_mask,
            .canonical_locations = cmd->state.shaders[MESA_SHADER_VERTEX]->info.conservative_canonical_outputs,
         };
         params.capture = gfx->polygon_capture =
            kk_pool_upload(cmd, &conservative, sizeof(conservative), 8).gpu;
      } else {
         params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;
      }`);
    s = s.replace('   bool point = vs->info.polygon_point &&', `   bool conservative = vs->info.conservative && dyn->rs.conservative_mode ==
      VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
   if (conservative != gfx->conservative) {
      gfx->conservative = conservative;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
   }
   bool point = conservative || (vs->info.polygon_point &&`);
    s = s.replace('(dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias);', '(dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias));');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s.replace(`   libkk_polygon_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));
   libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);
   libkk_polygon_output_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));
   libkk_polygon_output(cmd, triangles, true, geometry_params);`, `   libkk_polygon_clip(cmd, triangles, true, geometry_params, kk_heap(cmd));
   if (gfx->conservative)
      libkk_conservative_prepare(cmd, triangles, true, geometry_params, kk_heap(cmd));
   libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);
   if (gfx->conservative) {
      libkk_conservative_output_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));
      libkk_conservative_output(cmd, triangles, true, geometry_params);
   } else {
      libkk_polygon_output_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));
      libkk_polygon_output(cmd, triangles, true, geometry_params);
   }`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .EXT_line_rasterization = true,', '      .EXT_line_rasterization = true,\n      .EXT_conservative_rasterization = true,')
    .replace('      .extendedDynamicState3PolygonMode = true,', `      .extendedDynamicState3PolygonMode = true,
      .extendedDynamicState3ConservativeRasterizationMode = true,
      .extendedDynamicState3ExtraPrimitiveOverestimationSize = true,`)
    .replace('   *properties = (struct vk_properties){', `   *properties = (struct vk_properties){
      .primitiveOverestimationSize = 1.0f / 256.0f,
      .maxExtraPrimitiveOverestimationSize = 0.0f,
      .extraPrimitiveOverestimationSizeGranularity = 0.0f,
      .primitiveUnderestimation = false,
      .conservativePointAndLineRasterization = false,
      .degenerateTrianglesRasterized = false,
      .degenerateLinesRasterized = false,
      .fullyCoveredFragmentShaderInputVariable = false,
      .conservativeRasterizationPostDepthCoverage = false,`));
}
