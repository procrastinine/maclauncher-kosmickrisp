// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'dynamic-multisample';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-dynamic-multisample.h'];
export function apply({edit, edits}) {
  const replace = (s, from, to) => {
    if (!s.includes(from)) throw Error('Missing dynamic-multisample anchor: ' + from.slice(0, 100));
    return s.replace(from, to);
  };
  edits.set('src/kosmickrisp/vulkan/kk_dynamic_multisample.h',
    fs.readFileSync(new URL('./kosmickrisp-dynamic-multisample.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => replace(s,
    '         uint64_t vertex_input;',
    '         uint64_t vertex_input;\n         uint32_t sample_mask;\n         uint32_t alpha_to_coverage;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = replace(s, '   bool sample_shading_enable;\n   bool has_depth;',
      '   bool sample_shading_enable;\n   bool alpha_to_coverage_enable;\n   bool alpha_to_one_enable;\n   uint8_t dynamic_multisample;\n   bool has_depth;');
    s = replace(s, '      key->sample_shading_enable = state->ms->sample_shading_enable;',
      `      key->sample_shading_enable = state->ms->sample_shading_enable;
      key->alpha_to_coverage_enable = state->ms->alpha_to_coverage_enable;
      key->alpha_to_one_enable = state->ms->alpha_to_one_enable;`);
    s = replace(s, '   /* Depth writes are removed unless there\'s an actual attachment */',
      `   key->dynamic_multisample =
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES) |
      (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK) << 1) |
      (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE) << 2);

   /* Depth writes are removed unless there's an actual attachment */`);
    s = replace(s, 'static void\nkk_lower_fs(struct kk_device *dev, nir_shader *nir,',
      '#include "kk_dynamic_multisample.h"\n\nstatic void\nkk_lower_fs(struct kk_device *dev, nir_shader *nir,');
    s = replace(s, `   if (state->ms) {
      NIR_PASS(_, nir, msl_nir_lower_multisample_alpha,
               state->ms->alpha_to_coverage_enable,
               state->ms->alpha_to_one_enable);
   }`, '   kk_lower_multisample_alpha(nir, state);');
    s = replace(s, `   if (state->ms && state->ms->rasterization_samples &&
       state->ms->sample_mask != UINT16_MAX) {`, '   if (kk_fs_has_sample_mask(state)) {');
    s = replace(s, '      NIR_PASS(_, nir, msl_lower_static_sample_mask, state->ms->sample_mask);',
      '      kk_lower_sample_mask(nir, state);');
    s = replace(s, '      info->vs.sample_count = ms->rasterization_samples;',
      '      info->vs.sample_count = MAX2(ms->rasterization_samples, 1);');
    s = replace(s, '   if (render && device->vk.enabled_features.dynamicRenderingUnusedAttachments) {',
      `   if (render && (device->vk.enabled_features.dynamicRenderingUnusedAttachments ||
                  device->vk.enabled_features.extendedDynamicState3RasterizationSamples)) {`);
    return s;
  });
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => replace(s,
    'bool msl_lower_static_sample_mask(nir_shader *nir, uint32_t sample_mask);',
    `bool msl_lower_static_sample_mask(nir_shader *nir, uint32_t sample_mask);
bool msl_lower_sample_mask(nir_shader *nir, nir_def *sample_mask);
bool msl_nir_lower_multisample_alpha_dynamic(nir_shader *nir, bool coverage,
                                            bool one, nir_def *enable);`));
  edit('src/kosmickrisp/compiler/msl_nir_lower_common.c', s => {
    s = replace(s, `msl_lower_static_sample_mask(nir_shader *nir, uint32_t sample_mask)
{`, `msl_lower_sample_mask(nir_shader *nir, nir_def *static_sample_mask)
{`);
    s = replace(s, '   nir_builder b = nir_builder_at(nir_before_impl(entrypoint));\n\n   struct nir_io_semantics io_semantics',
      '   nir_builder b = nir_builder_at(nir_after_instr(nir_def_instr(static_sample_mask)));\n\n   struct nir_io_semantics io_semantics');
    s = replace(s, '   nir_function_impl *entrypoint = nir_shader_get_entrypoint(nir);\n   nir_builder b = nir_builder_at(nir_after_instr(nir_def_instr(static_sample_mask)));',
      '   nir_builder b = nir_builder_at(nir_after_instr(nir_def_instr(static_sample_mask)));');
    s = replace(s, '   nir_def *static_sample_mask = nir_imm_int(&b, sample_mask);\n', '');
    s = replace(s, 'static bool\nis_color_output(const nir_intrinsic_instr *intr)',
      `bool
msl_lower_static_sample_mask(nir_shader *nir, uint32_t sample_mask)
{
   nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
   return msl_lower_sample_mask(nir, nir_imm_int(&b, sample_mask));
}

static bool
is_color_output(const nir_intrinsic_instr *intr)`);
    s = replace(s, `msl_nir_lower_multisample_alpha(nir_shader *s, bool alpha_to_coverage,
                                bool alpha_to_one)`,
      `msl_nir_lower_multisample_alpha_dynamic(nir_shader *s, bool alpha_to_coverage,
                                        bool alpha_to_one, nir_def *enable)`);
    s = replace(s, 'NIR_PASS(progress, s, nir_lower_alpha_to_coverage, false,\n               NULL);',
      'NIR_PASS(progress, s, nir_lower_alpha_to_coverage, false, enable);');
    return s + `
bool
msl_nir_lower_multisample_alpha(nir_shader *nir, bool coverage, bool one)
{
   return msl_nir_lower_multisample_alpha_dynamic(nir, coverage, one, NULL);
}
`;
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
    s = replace(s, '   if (IS_DIRTY(IA_PRIMITIVE_TOPOLOGY) || IS_DIRTY(RS_LINE_MODE)) {',
      '   if (IS_DIRTY(IA_PRIMITIVE_TOPOLOGY) || IS_DIRTY(RS_LINE_MODE) ||\n       IS_DIRTY(MS_RASTERIZATION_SAMPLES)) {');
    s = replace(s, '   bool needs_restart = false;\n   /* Metal fixes the default sample count',
      `   cmd->state.gfx.pipeline_sample_count = MAX2(dyn->ms.rasterization_samples, 1);
   bool needs_restart = false;
   /* Metal fixes the default sample count`);
    s = replace(s, '       IS_DIRTY(CB_COLOR_WRITE_ENABLES) || IS_DIRTY(COLOR_ATTACHMENT_MAP)) {',
      `       IS_DIRTY(CB_COLOR_WRITE_ENABLES) || IS_DIRTY(COLOR_ATTACHMENT_MAP) ||
       IS_DIRTY(MS_RASTERIZATION_SAMPLES)) {`);
    s = replace(s, '   if (desc->root_dirty)\n      kk_upload_descriptor_root(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);',
      `   if (IS_DIRTY(MS_SAMPLE_MASK) || IS_DIRTY(MS_ALPHA_TO_COVERAGE_ENABLE) ||
       IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(FRAGMENT)) {
      desc->root.draw.sample_mask = dyn->ms.sample_mask;
      desc->root.draw.alpha_to_coverage = dyn->ms.alpha_to_coverage_enable;
      desc->root_dirty = true;
   }

   if (desc->root_dirty)
      kk_upload_descriptor_root(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_unused_attachments.h', s => {
    s = replace(s, '   bool polygon;\n   mtl_render_pipeline_state *pipeline;',
      '   bool polygon;\n   uint32_t samples;\n   mtl_render_pipeline_state *pipeline;');
    s = replace(s, '   if (!memcmp(formats, shader->info.vs.rt_formats, sizeof(formats)))',
      `   uint32_t samples = MAX2(cmd->vk.dynamic_graphics_state.ms.rasterization_samples, 1);
   if (samples == shader->info.vs.sample_count &&
       !memcmp(formats, shader->info.vs.rt_formats, sizeof(formats)))`);
    s = replace(s, '      if (v->polygon == polygon && !memcmp(v->formats, formats, sizeof(formats))) {',
      '      if (v->polygon == polygon && v->samples == samples &&\n          !memcmp(v->formats, formats, sizeof(formats))) {');
    s = replace(s, '         cache->descriptor[polygon], formats, MAX_DRAW_BUFFERS, disabled);',
      '         cache->descriptor[polygon], formats, MAX_DRAW_BUFFERS, disabled, samples);');
    s = replace(s, '         variant->polygon = polygon;',
      '         variant->polygon = polygon;\n         variant->samples = samples;');
    return s;
  });
  edit('src/kosmickrisp/bridge/mtl_compiler.h', s => replace(s,
    '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled);',
    '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,\n   uint32_t samples);'));
  edit('src/kosmickrisp/bridge/mtl_compiler.m', s => {
    s = replace(s, '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled)\n{',
      '   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,\n   uint32_t samples)\n{');
    return replace(s, '      MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)descriptor copy];',
      '      MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)descriptor copy];\n      copy.rasterSampleCount = samples;');
  });
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => replace(s,
    '      .extendedDynamicState3DepthClampEnable = true,',
    `      .extendedDynamicState3RasterizationSamples = true,
      .extendedDynamicState3SampleMask = true,
      .extendedDynamicState3AlphaToCoverageEnable = true,
      .extendedDynamicState3DepthClampEnable = true,`));
}
