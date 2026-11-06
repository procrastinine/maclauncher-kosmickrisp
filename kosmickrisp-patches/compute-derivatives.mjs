// SPDX-License-Identifier: MIT
import fs from 'node:fs';

/** Linear compute derivative groups use Metal's consecutive quad lanes. */
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-compute-derivatives.h'];
export function apply({edit: editSource, edits}) {
  const edit = (name, transform) => editSource('src/kosmickrisp/' + name, transform);
  edit('vulkan/kk_physical_device.c', s => s
    .replace('.KHR_shader_atomic_int64 = false,', '.KHR_compute_shader_derivatives = true,\n      .KHR_shader_atomic_int64 = false,')
    .replace('      .robustBufferAccess = true,', `      .computeDerivativeGroupLinear = true,
      .computeDerivativeGroupQuads = false,
      .robustBufferAccess = true,`));
  edit('vulkan/kk_shader.c', s => s
    .replace('#include "poly/nir/poly_nir.h"', '#include "poly/nir/poly_nir.h"\n#include "kosmickrisp/compiler/msl_compute_derivatives.h"')
    .replace('   NIR_PASS(_, nir, nir_lower_global_vars_to_local);',
      '   kk_lower_compute_derivatives(nir);\n   NIR_PASS(_, nir, nir_lower_global_vars_to_local);'));
  edit('vulkan/kk_descriptor_types.h', s => s
    .replace('   uint16_t pad_to_64_bits;', '   uint16_t sampler_mipmap_mode;')
    .replace('   uint64_t pad_to_power_2[3];',
      '   uint16_t sampler_query_lod_bias_fp16;\n   uint16_t sampler_pad[3];\n   uint64_t pad_to_power_2[2];')
    .replace('static_assert(sizeof(struct kk_sampled_image_descriptor)',
      `static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) == 18,
              "image minimum LOD retains its upstream field");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) == 20,
              "integer image minimum LOD retains its upstream field");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode) == 22,
              "mipmap mode occupies the upstream alignment padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_query_lod_bias_fp16) == 40,
              "query bias occupies the upstream tail padding");
static_assert(sizeof(struct kk_sampled_image_descriptor)`));
  edit('vulkan/kk_sampler.h', s => s.replace('   uint16_t lod_max_fp16;',
    '   uint16_t lod_max_fp16;\n   uint16_t query_lod_bias_fp16;\n   uint16_t mipmap_mode;'));
  edit('vulkan/kk_sampler.c', s => s.replace(
    '   sampler->lod_min_fp16 = _mesa_float_to_half(pCreateInfo->minLod);',
    `   sampler->query_lod_bias_fp16 = _mesa_float_to_half(pCreateInfo->mipLodBias);
   sampler->mipmap_mode = pCreateInfo->mipmapMode;
   sampler->lod_min_fp16 = _mesa_float_to_half(pCreateInfo->minLod);`));
  edit('vulkan/kk_descriptor_set.c', s => s
    .replace('         desc[plane].sampler_lod_max_fp16 = sampler->lod_max_fp16;',
      `         desc[plane].sampler_lod_max_fp16 = sampler->lod_max_fp16;
         desc[plane].sampler_mipmap_mode = sampler->mipmap_mode;
         desc[plane].sampler_query_lod_bias_fp16 =
            sampler->query_lod_bias_fp16;`));
  edit('vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '      nir_tex_instr_add_src(tex, nir_tex_src_max_lod_kk, lod_max);',
    `      nir_tex_instr_add_src(tex, nir_tex_src_max_lod_kk, lod_max);
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
      }`));
  edits.set('src/kosmickrisp/compiler/msl_compute_derivatives.h',
    fs.readFileSync(new URL('./kosmickrisp-compute-derivatives.h', import.meta.url), 'utf8'));
}
