// SPDX-License-Identifier: MIT
export const id = 'native-fragment-finish';

/** Finish native fragment alternatives through the ordinary fragment path. */
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('#include "kk_sparse_pipeline.h"\n\nstatic VkResult\nkk_compile_shaders_native(',
      `#include "kk_sparse_pipeline.h"

static void
kk_finish_fragment_nir(struct kk_device *dev, nir_shader *fs,
                       const struct vk_graphics_pipeline_state *state,
                       const struct vk_shader_compile_info *info,
                       kk_feature_key features, nir_shader **retained)
{
   bool callable = kk_nir_vrs_split_application(fs);
   kk_lower_fs(dev, fs, state);
   kk_finish_nir(dev, fs, info ? info->robustness : &rs_none,
      info ? info->set_layout_count : 0, info ? info->set_layouts : NULL, features);
   /* The shared embedded schema carries original-stage root loads; its
    * canonical sampler lifetime remains with the owning fine shader. */
   static_assert(KK_EMBEDDED_SAMPLER_BINDING_SCHEMA == 1u,
                 "retained embedded source uses portable runtime root bindings");
   if (retained && dev->vrs && callable)
      *retained = nir_shader_clone(NULL, fs);
   kk_nir_vrs_flatten(fs);
   kk_nir_vrs_lower_fine_rate(fs);
   nir_opts(fs, NULL);
   nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));
}

static VkResult
kk_compile_shaders_native(`);
    s = s.replace('   nir_shader *native_pair[2] = {NULL, NULL};',
      '   nir_shader *native_pair[2] = {NULL, NULL};\n' +
      '   bool native_fragment_needs_finish = false;');
    s = s.replace(`      native_pair[1] = native_fs ? nir_shader_clone(NULL, native_fs) :
         get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);`,
      `      native_pair[1] = native_fs ? nir_shader_clone(NULL, native_fs) :
         get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);
      native_fragment_needs_finish = !native_fs || native_fs == fs_to_finish;`);
    s = s.replace(`      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);`,
      `      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
      /* This clone predates deferred fragment finishing. Use its original
       * descriptor layouts, and leave already-finished incoming IR alone. */
      if (native_fragment_needs_finish) {
         const struct vk_shader_compile_info *info = NULL;
         for (unsigned i = 0; i < shader_count; ++i)
            if (infos[i].stage == MESA_SHADER_FRAGMENT) info = &infos[i];
         kk_finish_fragment_nir(dev, native_pair[1], state, info, features, NULL);
      }`);
    s = s.replace(`      /* One callable program receives the fine epilogue and descriptor tail.
       * Retention copies only finished source; compilation flattens it once. */
      bool callable = kk_nir_vrs_split_application(fs);
      kk_lower_fs(dev, fs, state);
      kk_finish_nir(dev, fs, info ? info->robustness : &rs_none,
         info ? info->set_layout_count : 0, info ? info->set_layouts : NULL, features);
      if (dev->vrs && callable && (!info || !info->embedded_sampler_count))
         vrs_sources[i] = nir_shader_clone(NULL, fs);
      kk_nir_vrs_flatten(fs);
      kk_nir_vrs_lower_fine_rate(fs);
      nir_opts(fs, NULL);
      nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));`,
      `      /* Main and native alternatives share the same fine epilogue and
       * descriptor finishing; only the main program retains VRS source. */
      kk_finish_fragment_nir(dev, fs, state, info, features, &vrs_sources[i]);`);
    return s;
  });
}
