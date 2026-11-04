/* SPDX-License-Identifier: MIT */
static bool
kk_fs_has_sample_mask(const struct vk_graphics_pipeline_state *state)
{
   if (!state)
      return false;
   if (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK))
      return true;
   if (!state->ms)
      return false;

   /* Bits above the static sample count cannot affect coverage. For dynamic
    * counts retain the complete mask, since later draws can use more samples. */
   uint32_t active = UINT16_MAX;
   if (!BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES)) {
      unsigned samples = MIN2(MAX2(state->ms->rasterization_samples, 1u), 16u);
      active = BITFIELD_MASK(samples);
   }
   return (state->ms->sample_mask & active) != active;
}

static nir_def *
kk_fs_multisample_value(nir_builder *b, uint64_t offset)
{
   nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);
   return nir_load_global_constant(b, 1, 32, nir_iadd_imm(b, root, offset));
}

static void
kk_lower_multisample_alpha(nir_shader *nir,
                          const struct vk_graphics_pipeline_state *state)
{
   bool dynamic = BITSET_TEST(state->dynamic,
                              MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE);
   bool coverage = dynamic || (state->ms && state->ms->alpha_to_coverage_enable);
   bool one = state->ms && state->ms->alpha_to_one_enable;
   nir_def *enable = NULL;
   if (dynamic) {
      nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
      enable = nir_ine_imm(&b, kk_fs_multisample_value(&b,
         offsetof(struct kk_root_descriptor_table, draw.alpha_to_coverage)), 0);
   }
   msl_nir_lower_multisample_alpha_dynamic(nir, coverage, one, enable);
}

static void
kk_lower_sample_mask(nir_shader *nir,
                     const struct vk_graphics_pipeline_state *state)
{
   if (BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_SAMPLE_MASK)) {
      nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
      nir_def *mask = kk_fs_multisample_value(&b,
         offsetof(struct kk_root_descriptor_table, draw.sample_mask));
      msl_lower_sample_mask(nir, mask);
   } else {
      msl_lower_static_sample_mask(nir, state->ms->sample_mask);
   }
}
