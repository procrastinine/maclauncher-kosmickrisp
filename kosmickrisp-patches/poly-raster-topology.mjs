// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'poly-raster-topology';
export const sourceFiles = ['kosmickrisp-patches/poly-raster-topology.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_poly_raster_topology.h',
    fs.readFileSync(new URL('./poly-raster-topology.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
    .replace('#include "poly/nir/poly_nir.h"',
      '#include "poly/nir/poly_nir.h"\n#include "kk_poly_raster_topology.h"')
    .replace('         enum mtl_primitive_topology_class topology;',
      '         enum mtl_primitive_topology_class topology;\n         enum mtl_primitive_topology_class polygon_topology;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('      KK_FEAT_COUNT, kk_make_feature_key(enabled_features),',
      '      KK_FEAT_COUNT, sizeof(struct kk_shader_info), kk_make_feature_key(enabled_features),')
    .replace('   /* Representation changes whether a Poly raster pipeline is required.',
      `   /* Serialized producer topology is pipeline state, including when the
    * same shader body is reused by different raster classes. */
   if (state && state->rs && state->ia) {
      const uint32_t raster_key[] = {
         state->ia->primitive_topology, state->rs->polygon_mode, state->rs->line.mode,
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE),
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE),
      };
      _mesa_blake3_update(&blake3_ctx, raster_key, sizeof(raster_key));
   }

   /* Representation changes whether a Poly raster pipeline is required.`)
    .replace('      vk_primitive_topology_to_mtl_primitive_topology_class(topology);',
      `      vk_primitive_topology_to_mtl_primitive_topology_class(topology);
   info->vs.polygon_topology = info->polygon_point ?
      kk_polygon_raster_topology(info->polygon.primitive_vertices,
         state->rs->polygon_mode, state->rs->line.mode) : info->vs.topology;`)
    .replace('!device->vk.enabled_features.drawIndirectCount)\n      return VK_SUCCESS;',
      '!device->vk.enabled_features.drawIndirectCount &&\n       !shader->info.polygon_point && !shader->info.poly_layer_replay)\n      return VK_SUCCESS;')
    .replace('   if (!cache->descriptor[kind])\n      cache->descriptor[kind] = mtl_retain(descriptor);',
      `   if (!cache->descriptor[kind]) {
      cache->descriptor[kind] = mtl_retain(descriptor);
      cache->topology[kind] = kind == KK_PIPELINE_CONSERVATIVE ?
         MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE : kind == KK_PIPELINE_POLYGON ?
         shader->info.vs.polygon_topology : shader->info.vs.topology;
   }`)
    .replace('         polygon ? MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED : vs->info.vs.topology);',
      '         polygon ? vs->info.vs.polygon_topology : vs->info.vs.topology);'));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s.replace(
    `      .primitive_type = vs->info.polygon.primitive_vertices == 1 ? MTL_PRIMITIVE_TYPE_POINT :
         (vs->info.polygon.primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE) &&
         dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM ?
         MTL_PRIMITIVE_TYPE_LINE : MTL_PRIMITIVE_TYPE_TRIANGLE,`,
    `      .primitive_type = kk_polygon_raster_primitive(vs->info.polygon.primitive_vertices,
         dyn->rs.polygon_mode, dyn->rs.line.mode),`));
  edit('src/kosmickrisp/vulkan/kk_unused_attachments.h', s => s
    .replace('   unsigned kind;\n   uint32_t samples;',
      '   unsigned kind;\n   enum mtl_primitive_topology_class topology;\n   uint32_t samples;')
    .replace('   mtl_render_pipeline_descriptor *descriptor[KK_PIPELINE_KIND_COUNT];',
      '   mtl_render_pipeline_descriptor *descriptor[KK_PIPELINE_KIND_COUNT];\n   enum mtl_primitive_topology_class topology[KK_PIPELINE_KIND_COUNT];')
    .replace('   bool indirect, VkResult *error, uint32_t samples)',
      '   bool indirect, VkResult *error, uint32_t samples,\n   enum mtl_primitive_topology_class topology)')
    .replace('   if (!indirect && samples == shader->info.vs.sample_count &&',
      '   if (!indirect && topology == cache->topology[kind] &&\n       samples == shader->info.vs.sample_count &&')
    .replace('v->indirect == indirect && v->samples == samples &&',
      'v->indirect == indirect && v->samples == samples &&\n          v->topology == topology &&')
    .replace('      variant->pipeline = mtl_new_render_pipeline_for_attachments(',
      '      variant->pipeline = mtl_new_render_pipeline_for_raster_attachments(')
    .replace('         shader->info.has_framebuffer_reads);',
      '         shader->info.has_framebuffer_reads, topology);')
    .replace('         variant->kind = kind;',
      '         variant->kind = kind;\n         variant->topology = topology;')
    .replace('   VkResult error = VK_SUCCESS;\n   mtl_render_pipeline_state *pipeline =',
      `   enum mtl_primitive_topology_class topology = kind == KK_PIPELINE_CONSERVATIVE ?
      MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE : kind == KK_PIPELINE_POLYGON && shader->info.polygon_point ?
      kk_polygon_raster_topology(shader->info.polygon.primitive_vertices,
         cmd->vk.dynamic_graphics_state.rs.polygon_mode,
         cmd->vk.dynamic_graphics_state.rs.line.mode) : shader->info.vs.topology;
   VkResult error = VK_SUCCESS;
   mtl_render_pipeline_state *pipeline =`)
    .replace('      cmd->state.gfx.dgc_icb, &error, samples);',
      '      cmd->state.gfx.dgc_icb, &error, samples, topology);')
    .replace('shader->info.vs.sample_count))\n         return error;',
      'shader->info.vs.sample_count, cache->topology[kind]))\n         return error;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    '       IS_DIRTY(MS_RASTERIZATION_SAMPLES)) {\n      struct kk_shader *vs = cmd->state.gfx.shader;',
    '       IS_DIRTY(MS_RASTERIZATION_SAMPLES) || IS_DIRTY(RS_LINE_MODE)) {\n      struct kk_shader *vs = cmd->state.gfx.shader;'));
  /* Specialize only the already-copied native descriptor. The cached original
   * remains immutable, including its linking state and original topology. */
  edit('src/kosmickrisp/bridge/mtl_compiler.h', s => s.replace(
    'void mtl_render_pipeline_descriptor_copy_linking(',
    `mtl_render_pipeline_state *mtl_new_render_pipeline_for_raster_attachments(
   mtl_compiler *compiler, mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,
   uint32_t samples, bool framebuffer_reads, enum mtl_primitive_topology_class topology);

void mtl_render_pipeline_descriptor_copy_linking(`));
  edit('src/kosmickrisp/bridge/mtl_compiler.m', s => s
    .replace('mtl_new_render_pipeline_for_attachments(mtl_compiler *compiler,',
      'mtl_new_render_pipeline_for_raster_attachments(mtl_compiler *compiler,')
    .replace('   uint32_t samples, bool framebuffer_reads)',
      '   uint32_t samples, bool framebuffer_reads, enum mtl_primitive_topology_class topology)')
    .replace('      copy.rasterSampleCount = samples;',
      `      copy.rasterSampleCount = samples;
      if (topology != MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED)
         copy.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)topology;`)
    .replace('void\nmtl_render_pipeline_descriptor_copy_linking(',
      `/* Existing consumers retain the topology already set on their descriptor. */
mtl_render_pipeline_state *
mtl_new_render_pipeline_for_attachments(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,
   uint32_t samples, bool framebuffer_reads)
{
   return mtl_new_render_pipeline_for_raster_attachments(compiler, descriptor,
      formats, count, disabled, samples, framebuffer_reads,
      MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED);
}

void
mtl_render_pipeline_descriptor_copy_linking(`));
}
