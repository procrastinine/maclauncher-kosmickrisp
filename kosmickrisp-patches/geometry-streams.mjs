// SPDX-License-Identifier: MIT
// Raster stream state follows Willie Abrams's Mesa MR !44928, MIT:
// https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44928
// head 88bb97ae268c0d0ad494ca7a990f80367301c95c (2026-11-05).
// The existing Poly XFB passes are retained; simultaneous generated queries
// additionally need independent destinations and complete 64-bit reports.
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    '   uint32_t provoking_last;',
    '   uint32_t provoking_last;\n   uint32_t rasterization_stream;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('   if (IS_DIRTY(RS_RASTERIZER_DISCARD_ENABLE)) {\n      if (dyn->rs.rasterizer_discard_enable) {',
      `   const struct kk_shader *gs = kk_geometry_shader(cmd);
   bool rast_discard = dyn->rs.rasterizer_discard_enable ||
      (dyn->rs.rasterization_stream != 0 && !(gs && gs->info.geometry.multistream));
   if (IS_DIRTY(RS_RASTERIZER_DISCARD_ENABLE) ||
       IS_DIRTY(RS_RASTERIZATION_STREAM) || IS_SHADER_DIRTY(VERTEX) ||
       IS_SHADER_DIRTY(GEOMETRY)) {
      if (rast_discard) {`)
    .replace('   if (!(dyn->rs.rasterizer_discard_enable || gfx->is_cull_front_and_back) &&',
      '   if (!(rast_discard || gfx->is_cull_front_and_back) &&')
    .replace('      gfx->per_draw_data.provoking_last =',
      '      gfx->per_draw_data.rasterization_stream = dyn->rs.rasterization_stream;\n      gfx->per_draw_data.provoking_last =')
);
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '   case nir_intrinsic_load_provoking_last:',
    '   case nir_intrinsic_load_rasterization_stream:\n      return lower_sysval_to_per_draw(b, intrin, rasterization_stream);\n   case nir_intrinsic_load_provoking_last:'));

  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .geometryStreams = false,', '      .geometryStreams = true,')
    .replace('      .primitivesGeneratedQueryWithNonZeroStreams = false,',
      '      .primitivesGeneratedQueryWithNonZeroStreams = true,')
    .replace('      .transformFeedbackRasterizationStreamSelect = false,',
      '      .transformFeedbackRasterizationStreamSelect = true,'));
}
