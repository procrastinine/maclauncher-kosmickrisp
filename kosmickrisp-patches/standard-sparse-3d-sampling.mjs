// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export function apply({edit,edits}) {
 edits.set('src/kosmickrisp/vulkan/kk_standard_sparse_3d_sample.h',
  fs.readFileSync(new URL('./standard-sparse-3d-sample.h',import.meta.url),'utf8'));
 edit('src/kosmickrisp/vulkan/kk_shader.h',s=>s.replace(
  'bool kk_nir_lower_sparse(nir_shader *nir);',
  'bool kk_nir_lower_sparse(nir_shader *nir);\nbool kk_nir_lower_standard_3d_samples(nir_shader *,uint32_t,struct vk_descriptor_set_layout *const *);'));
 edit('src/kosmickrisp/vulkan/kk_shader.c',s=>s.replace(
  '   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers, set_layout_count, set_layouts,',
  '   if (features & BITFIELD_BIT(KK_FEAT_SPARSE_BUFFER))\n      NIR_PASS(_, nir, kk_nir_lower_standard_3d_samples, set_layout_count, set_layouts);\n\n   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers, set_layout_count, set_layouts,'));
 edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c',s=>s.replace(
  '#include "kk_sparse_sample_nir.h"',
  '#include "kk_sparse_sample_nir.h"\n#include "kk_standard_sparse_3d_sample.h"'));
 // Extend the shared footprint builder; ordinary callers select its exact
 // original reduction path. Only standard-volume taps request weights.
 edit('src/kosmickrisp/vulkan/kk_sampler_minmax.h',s=>s
  .replace('                nir_def *linear, nir_def *mode, nir_def *unnormalized)',
   `                nir_def *linear, nir_def *mode, nir_def *unnormalized,
                bool weighted, nir_def *(*tap_cb)(nir_builder *,nir_tex_instr *,
                   nir_def *,nir_def *,void *), void *tap_data)`)
  .replace('      nir_def *center = nir_vec(b, components, dims);',
   `      nir_def *factor = NULL;
      if (weighted) {
         factor = nir_imm_float(b, 1);
         for (unsigned axis = 0; axis < dims; axis++) {
            nir_def *f = nir_channel(b, fraction, axis);
            if (!(corner & (1u << axis))) f = nir_fsub(b, nir_imm_float(b, 1), f);
            factor = nir_fmul(b, factor, f);
         }
         factor = nir_bcsel(b, linear, factor,
            nir_imm_float(b, corner == 0 ? 1 : 0));
      }
      nir_def *center = nir_vec(b, components, dims);`)
  .replace('         result = kk_minmax_tap(b, tex, center, level);',
   `         result = tap_cb ? tap_cb(b, tex, center, level, tap_data) :
                           kk_minmax_tap(b, tex, center, level);
         if (weighted && nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
            nir_def *color = tex->is_sparse ? nir_trim_vector(b, result, 4) : result;
            nir_def *scaled = nir_fmul(b, color, nir_f2fN(b, factor, color->bit_size));
            color = nir_bcsel(b, nir_ieq_imm(b, mode, 0), scaled, color);
            result = tex->is_sparse ? nir_vector_insert_imm(b,
               nir_pad_vector(b, color, 5), nir_channel(b, result, 4), 4) : color;
         }`)
  .replace('         nir_def *tap = kk_minmax_tap(b, tex, center, level);',
   `         nir_def *tap = tap_cb ? tap_cb(b, tex, center, level, tap_data) :
                              kk_minmax_tap(b, tex, center, level);`)
  .replace('         nir_def *combined = kk_minmax_combine(b, result, tap, mode, tex->is_sparse, tex->dest_type);',
   `         nir_def *combined = kk_minmax_combine(b, result, tap, mode, tex->is_sparse, tex->dest_type);
         if (weighted && nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
            nir_def *a = tex->is_sparse ? nir_trim_vector(b, result, 4) : result;
            nir_def *c = tex->is_sparse ? nir_trim_vector(b, tap, 4) : tap;
            nir_def *sum = nir_fadd(b, a, nir_fmul(b, c, nir_f2fN(b, factor, c->bit_size)));
            if (tex->is_sparse) sum = nir_vector_insert_imm(b, nir_pad_vector(b, sum, 5),
               nir_channel(b, combined, 4), 4);
            combined = nir_bcsel(b, nir_ieq_imm(b, mode, 0), sum, combined);
         }`)
  .replaceAll('kk_minmax_level(b, tex, low, linear, mode, unnormalized)',
   'kk_minmax_level(b, tex, low, linear, mode, unnormalized, false, NULL, NULL)')
  .replaceAll('kk_minmax_level(b, tex, next, linear, mode, unnormalized)',
   'kk_minmax_level(b, tex, next, linear, mode, unnormalized, false, NULL, NULL)'));
}
