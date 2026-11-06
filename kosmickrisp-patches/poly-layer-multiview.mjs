// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'poly-layer-multiview';
export const sourceFiles = ['kosmickrisp-patches/poly-layer-multiview-nir.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_poly_layer_multiview_nir.h',
    fs.readFileSync(new URL('./poly-layer-multiview-nir.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('#include "kk_poly_layer_replay_nir.h"',
      '#include "kk_poly_layer_replay_nir.h"\n#include "kk_poly_layer_multiview_nir.h"')
    .replace('   bool force_per_vertex = kk_barycentric_force_per_vertex(',
      `   /* Multiview supplies the last producer's effective Layer even when
    * the application has no Layer output. Mesh owns its canonical reader. */
   poly_layer_replay |= layer_producer != MESA_SHADER_NONE && state &&
      state->mv && state->mv->view_mask;
   bool force_per_vertex = kk_barycentric_force_per_vertex(`)
    .replace('      assert(producer && state);',
      `      assert(producer && state);
      /* The native alternative was cloned above with ordinary amplification.
       * Only its captured sibling consumes the existing per-view uniform. */
      if (state->mv->view_mask && producer->info.stage == MESA_SHADER_VERTEX)
         kk_poly_layer_capture_view(producer, state->mv->view_mask);`)
    .replace('   if (feats->shaderOutputLayer) key |= BITFIELD_BIT(KK_FEAT_POLY_LAYER_REPLAY);',
      '   if (feats->shaderOutputLayer || feats->multiview)\n      key |= BITFIELD_BIT(KK_FEAT_POLY_LAYER_REPLAY);'));
  edit('src/kosmickrisp/vulkan/kk_poly_layer_replay_command.h', s => s
    .replace('          dev->vk.enabled_features.geometryShader;',
      '          dev->vk.enabled_features.geometryShader ||\n          dev->vk.enabled_features.multiview;')
    .replace('   /* ViewIndex multiplicity and Mesh canonical packets retain their own\n    * interfaces; this hook does not replace either producer contract. */',
      `   /* The outer Poly loop supplies one actual view at a time. Captured
    * stages and their original queries execute once for that view, before
    * any physical-slice raster replay. Mesh owns its separate entry. */`)
    .replace('gfx->layer_replay.bind || gfx->render.view_mask ||',
      'gfx->layer_replay.bind ||\n       (gfx->render.view_mask & (gfx->render.view_mask - 1u)) ||')
    .replace('   uint32_t layers = vs->info.poly_layer_replay ? replay.layer_count : 1u;\n   for (uint32_t layer = 0; layer < layers; ++layer) {',
      `   uint32_t view_mask = gfx->render.view_mask;
   assert(!(view_mask & (view_mask - 1u)));
   uint32_t first = view_mask ? ffs(view_mask) - 1u : 0u;
   uint32_t layers = view_mask ? 1u :
      (vs->info.poly_layer_replay ? replay.layer_count : 1u);
   for (uint32_t offset = 0; offset < layers; ++offset) {
      uint32_t layer = first + offset;`)
    .replace('      kk_dispatch_draw(cmd, draw);\n   }',
      `      /* Attachment aliases have physical index zero. Never amplify the
       * cached draw again after the original view has been produced. */
      if (view_mask) {
         uint32_t physical_layer = 0;
         mtl_set_vertex_amplification_count(cs_get_render(cmd), &physical_layer, 1);
      }
      kk_dispatch_draw(cmd, draw);
   }`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('       cmd->state.gfx.shader->info.api_geometry);\n   uint32_t per_draw_size',
      `       cmd->state.gfx.shader->info.api_geometry ||
       gfx->layer_replay.bind || gfx->polygon_points);
   uint32_t per_draw_size`)
    .replace('   if (!view_mask || !(vs->info.api_geometry ||\n                      cmd->state.shaders[MESA_SHADER_TESS_EVAL])) {',
      `   bool cached_views = vs->info.poly_layer_replay &&
      (gfx->render.layered_bc || kk_polygon_expansion_active(cmd) ||
       !vs->info.polygon_native);
   if (!view_mask || !(vs->info.api_geometry || cached_views ||
                      cmd->state.shaders[MESA_SHADER_TESS_EVAL])) {`)
    .replace('         if (tess || vs->info.api_geometry) {',
      '         if (tess || vs->info.api_geometry || gfx->layer_replay.bind ||\n             (vs->info.poly_layer_replay && gfx->polygon_points)) {'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s
    .replace('   if (view_mask && vs && (vs->info.api_geometry ||\n       cmd->state.shaders[MESA_SHADER_TESS_EVAL]))',
      '   if (view_mask && vs && (vs->info.api_geometry ||\n       state->layer_replay.bind ||\n       (vs->info.poly_layer_replay && state->polygon_points) ||\n       cmd->state.shaders[MESA_SHADER_TESS_EVAL]))')
    .replace('            .poly_layer_replay = dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader,',
      '            .poly_layer_replay = dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader || dev->vk.enabled_features.multiview,')
    .replace('      if (dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader)\n         mtl_set_address',
      '      if (dev->vk.enabled_features.shaderOutputLayer || dev->vk.enabled_features.geometryShader || dev->vk.enabled_features.multiview)\n         mtl_set_address'));
}
