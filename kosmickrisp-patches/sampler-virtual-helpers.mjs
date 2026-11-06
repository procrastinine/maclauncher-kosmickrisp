// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'sampler-virtual-helpers';
export const sourceFiles = ['kosmickrisp-patches/kk-sampler-params-nir.h'];
export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk-sampler-helpers-nir.h', s => s.replace(
    '   if (tex->is_shadow || !nir_tex_instr_need_sampler(tex))',
    '   if ((tex->backend_flags & KK_SAMPLE_METADATA) || !nir_tex_instr_need_sampler(tex))'));
  edits.set('src/kosmickrisp/compiler/kk-sampler-params-nir.h',
    fs.readFileSync(new URL('./kk-sampler-params-nir.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/compiler/msl_nir_lower_common.c', s => s.replace(
    'bool\nmsl_lower_textures(nir_shader *nir, bool lower_lod_bias)',
    '#include "kk-sampler-params-nir.h"\n\nbool\nmsl_lower_textures(nir_shader *nir, bool lower_lod_bias)').replace(
    '      .lower_sampler_lod_bias = lower_lod_bias,',
    '      .lower_sampler_lod_bias = false,').replace(
    '      .lower_txd_cube_map = true,',
    '      .lower_txd_cube_map = false,').replace(
    '   NIR_PASS(progress, nir, nir_lower_tex, &lower_tex_options);',
    '   NIR_PASS(progress, nir, kk_sampler_lower_bias, lower_lod_bias);\n   NIR_PASS(progress, nir, nir_lower_tex, &lower_tex_options);'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '#include "kk_sampler_minmax.h"',
    '#include "../compiler/kk-sampler-params-nir.h"\n#include "kk_sampler_minmax.h"').replace(
    '   kk_sample_descriptor(b, tex, sampler, plane_offset_B, ctx);',
    '   kk_sample_descriptor(b, tex, sampler, plane_offset_B, ctx);\n   if (tex->backend_flags & KK_SAMPLE_METADATA) return true;').replace(
    '   if (tex->op == nir_texop_lod) {\n      nir_def *lod_min',
    '   if (tex->op == nir_texop_lod && !(tex->backend_flags & KK_SAMPLE_HELPER)) {\n      nir_def *lod_min'));
  edit('src/kosmickrisp/compiler/msl_compute_derivatives.h', s => s.replace(
    '#include "nir.h"', '#include "nir.h"\n#include "kk-sampler-helper-flags.h"').replace(
    '            if (tex->op != nir_texop_lod)',
    '            if (tex->op != nir_texop_lod || (tex->backend_flags & KK_SAMPLE_HELPER))').replace(
    'static inline void\nkk_lower_compute_derivatives(nir_shader *nir)',
    'static bool\nkk_unhandled_cube_gradient(const nir_tex_instr *tex, UNUSED const void *data)\n{\n   return tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE &&\n          !(tex->backend_flags & KK_SAMPLE_HELPER);\n}\n\nstatic inline void\nkk_lower_compute_derivatives(nir_shader *nir)').replace(
    '   const nir_lower_tex_options cubes = {.lower_txd_cube_map = true};',
    '   const nir_lower_tex_options cubes = {.lower_txd_cb = kk_unhandled_cube_gradient};'));
  /* Hard dependency on Sparse fbadbda6: its complete metadata ABI replaces
   * the four historical half-field numeric substitutions. No optional anchor. */
  edit('src/kosmickrisp/compiler/kk_sparse_layout.h', s => {
    if (!s.includes('KK_SPARSE_ROW_PHYSICAL_FACTS_WORD') ||
        !s.includes('offsetof(struct kk_sparse_native_sampler_record, physical_facts) == 48'))
      throw new Error('sampler-virtual-helpers requires Sparse fbadbda6 physical-facts layout');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '(features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10,',
    'features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX),'));

  applySamplerParameterReaders(edit);
}

export function applySamplerParameterReaders(edit) {
  /* Original parameter pointers replace all four native numeric words.
   * The compiler's existing physical-device choice preserves native bias. */
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => s.replace(
    '   uint64_t disabled_workarounds;',
    '   uint64_t disabled_workarounds;\n   bool native_sampler_lod_bias;'));
  edit('src/kosmickrisp/compiler/msl_private.h', s => s.replace(
    '   uint64_t disabled_workarounds;',
    '   uint64_t disabled_workarounds;\n   bool native_sampler_lod_bias;'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s.replace(
    '      .disabled_workarounds = options->disabled_workarounds,',
    '      .disabled_workarounds = options->disabled_workarounds,\n      .native_sampler_lod_bias = options->native_sampler_lod_bias,'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '   translate_options.disabled_workarounds = pdev->settings.disabled_workarounds;',
    '   translate_options.disabled_workarounds = pdev->settings.disabled_workarounds;\n   translate_options.native_sampler_lod_bias = pdev->info.gpu_apple_family >= 10;'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s
    .replace(`   b->cursor = nir_before_instr(&tex->instr);

   nir_def *texture = nir_steal_tex_src(tex, nir_tex_src_texture_deref);`, `   b->cursor = nir_before_instr(&tex->instr);

   /* Old lod-bias queries use the same complete-metadata decoder too. */
   if (tex->op == nir_texop_lod_bias && !(tex->backend_flags & KK_SAMPLE_METADATA))
      tex->backend_flags |= KK_SAMPLE_METADATA | KK_SAMPLE_LOWER_NATIVE_BIAS |
         (KK_SAMPLER_META_BIAS << KK_SAMPLE_METADATA_SHIFT);

   if (tex->op == nir_texop_lod && !(tex->backend_flags & KK_SAMPLE_HELPER)) {
      nir_def *minimum = kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_MIN_LOD, false);
      nir_def *maximum = kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_MAX_LOD, false);
      lower_tex(b, nir_instr_as_tex(nir_def_instr(minimum)), ctx);
      lower_tex(b, nir_instr_as_tex(nir_def_instr(maximum)), ctx);
      nir_tex_instr_add_src(tex, nir_tex_src_min_lod, minimum);
      nir_tex_instr_add_src(tex, nir_tex_src_max_lod_kk, maximum);
      if (b->shader->info.stage == MESA_SHADER_COMPUTE &&
          b->shader->info.derivative_group == DERIVATIVE_GROUP_LINEAR) {
         b->cursor = nir_before_instr(&tex->instr);
         nir_def *bias = kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_QUERY_BIAS, false);
         nir_def *mode = kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_MIPMAP, false);
         lower_tex(b, nir_instr_as_tex(nir_def_instr(bias)), ctx);
         lower_tex(b, nir_instr_as_tex(nir_def_instr(mode)), ctx);
         b->cursor = nir_before_instr(&tex->instr);
         nir_tex_instr_add_src(tex, nir_tex_src_backend1,
            nir_vec4(b, nir_u2u32(b, nir_f2f16(b, bias)), nir_f2u32(b, mode),
                     nir_imm_int(b, 0), nir_imm_int(b, 0)));
      }
      b->cursor = nir_before_instr(&tex->instr);
   }

   nir_def *texture = nir_steal_tex_src(tex, nir_tex_src_texture_deref);`)
    .replace(`   if (tex->op == nir_texop_lod_bias) {
      unsigned offs =
         offsetof(struct kk_sampled_image_descriptor, sampler_lod_bias_fp16);

      nir_def *bias = load_resource_deref_desc(
         b, 1, 16, nir_src_as_deref(nir_src_for_ssa(sampler)),
         plane_offset_B + offs, ctx);

      nir_def_replace(&tex->def, bias);
      return true;
   }`, '')
    .replace(`   if (tex->op == nir_texop_lod && !(tex->backend_flags & KK_SAMPLE_HELPER)) {
      nir_def *lod_min = nir_f2f32(
         b, load_resource_deref_desc(
               b, 1, 16, nir_src_as_deref(nir_src_for_ssa(sampler)),
               plane_offset_B + offsetof(struct kk_sampled_image_descriptor,
                                         sampler_lod_min_fp16),
               ctx));
      nir_def *lod_max = nir_f2f32(
         b, load_resource_deref_desc(
               b, 1, 16, nir_src_as_deref(nir_src_for_ssa(sampler)),
               plane_offset_B + offsetof(struct kk_sampled_image_descriptor,
                                         sampler_lod_max_fp16),
               ctx));

      nir_tex_instr_add_src(tex, nir_tex_src_min_lod, lod_min);
      nir_tex_instr_add_src(tex, nir_tex_src_max_lod_kk, lod_max);
      if (b->shader->info.stage == MESA_SHADER_COMPUTE &&
          b->shader->info.derivative_group == DERIVATIVE_GROUP_LINEAR) {
         nir_deref_instr *s = nir_src_as_deref(nir_src_for_ssa(sampler));
         nir_def *bias = load_resource_deref_desc(b, 1, 16, s,
            plane_offset_B + offsetof(struct kk_sampled_image_descriptor,
                                      sampler_query_lod_bias_fp16), ctx);
         nir_def *mode = load_resource_deref_desc(b, 1, 16, s,
            plane_offset_B + offsetof(struct kk_sampled_image_descriptor,
                                      sampler_mipmap_mode), ctx);
         nir_tex_instr_add_src(tex, nir_tex_src_backend1,
            nir_vec4(b, nir_u2u32(b, bias), nir_u2u32(b, mode),
                      nir_imm_int(b, 0), nir_imm_int(b, 0)));
      }
   }`, ''));

}
