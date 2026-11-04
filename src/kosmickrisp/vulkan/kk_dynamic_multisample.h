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
   return nir_u2u32(b, nir_load_global_constant(
      b, 1, 8, nir_iadd_imm(b, root, offset)));
}

/* Match the primary color store consumed by nir_lower_alpha_to_coverage.
 * Opaque constant alpha covers every sample for every dynamic enable value.
 * Avoid introducing sample-ID/position reads and discard metadata for that
 * semantic identity; alpha-to-one still runs independently below. */
static bool
kk_fs_alpha_coverage_needed(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_instr_reverse(instr, nir_impl_last_block(impl)) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_store_output)
         continue;
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      if (sem.location != FRAG_RESULT_DATA0 || sem.dual_source_blend_index)
         continue;
      nir_def *rgba = intr->src[0].ssa;
      if (rgba->num_components < 4)
         return false;
      nir_scalar alpha = nir_scalar_resolved(rgba, 3);
      return !nir_scalar_is_const(alpha) || nir_scalar_as_float(alpha) < 1.0 ||
             isnan(nir_scalar_as_float(alpha));
   }
   return false;
}

static void
kk_lower_multisample_alpha(nir_shader *nir,
                          const struct vk_graphics_pipeline_state *state)
{
   bool dynamic = BITSET_TEST(state->dynamic,
                              MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE);
   bool coverage = (dynamic || (state->ms && state->ms->alpha_to_coverage_enable)) &&
                   kk_fs_alpha_coverage_needed(nir);
   bool one = state->ms && state->ms->alpha_to_one_enable;
   nir_def *enable = NULL;
   if (dynamic && coverage) {
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
