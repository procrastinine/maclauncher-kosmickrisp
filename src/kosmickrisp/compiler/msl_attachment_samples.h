/* SPDX-License-Identifier: MIT
 * Share native color inputs between ordinary and ordered attachment reads,
 * including multisampling and dynamically indexed attachment arrays.
 */
static bool
msl_attachment_sample_fetch(nir_builder *b, nir_intrinsic_instr *intr,
                            const struct lower_ia_state *state)
{
   /* Custom resolve reads the original multisampled source, not the bound
    * single-sample resolve target returned by a native color input. */
   if (state->rp->custom_resolve)
      return false;
   const bool multisample =
      nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_SUBPASS_MS;
   const bool constant = nir_src_is_const(intr->src[3]);
   const bool direct = constant &&
      (!multisample || state->interlock_shadow_samples);
   if (!multisample &&
       nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_SUBPASS)
      return false;
   const struct vk_input_attachment_location_state *ial = state->ial;
   const unsigned base = nir_intrinsic_base(intr);
   const unsigned range = nir_intrinsic_range(intr);
   unsigned index = base;
   if (constant)
      index += nir_src_as_uint(intr->src[3]);
   nir_def *result = &intr->def;
   for (unsigned i = 0; i < ial->color_attachment_count; ++i) {
      unsigned attachment = ial->color_map[i];
      unsigned slot = state->cal ? state->cal->color_map[i] : i;
      if (attachment == MESA_VK_ATTACHMENT_UNUSED ||
          slot == MESA_VK_ATTACHMENT_UNUSED ||
          i >= state->rp->color_attachment_count ||
          state->rp->color_attachment_formats[i] == VK_FORMAT_UNDEFINED ||
          (range && (attachment < base || attachment - base >= range)) ||
          attachment == ial->depth_att || attachment == ial->stencil_att ||
          (constant && attachment != index))
         continue;

      b->cursor = nir_before_instr(&intr->instr);
      nir_io_semantics semantics = {.location = FRAG_RESULT_DATA0 + slot,
                                   .num_slots = 1};
      nir_def *current = state->interlock_shadow_samples ?
         nir_load_tile_image(b, intr->def.num_components, intr->def.bit_size,
            nir_imm_int(b, 0), multisample ? intr->src[2].ssa : nir_imm_int(b, 0),
            .dest_type = nir_intrinsic_dest_type(intr), .io_semantics = semantics,
            .access = ACCESS_COHERENT) :
         nir_load_output(b, intr->def.num_components,
            intr->def.bit_size, nir_imm_int(b, 0),
            .dest_type = nir_intrinsic_dest_type(intr), .io_semantics = semantics);
      if (direct) {
         /* Preserve the existing last-matching-slot selection. */
         result = current;
         continue;
      }
      nir_def *matches = constant ? nir_imm_true(b) :
         nir_ieq_imm(b, nir_iadd_imm(b, intr->src[3].ssa, index), attachment);
      if (multisample && !state->interlock_shadow_samples) {
         matches = nir_iand(b, matches,
            nir_ieq(b, intr->src[2].ssa, nir_load_sample_id(b)));
      }
      b->cursor = nir_after_def(result);
      result = nir_bcsel(b, matches, current, result);
   }
   if (result == &intr->def)
      return false;
   if (direct) {
      /* Remove a known native read before generic lowering can construct its
       * unused texture read and descriptor fallback. */
      nir_def_replace(&intr->def, result);
      return true;
   }
   /* The implicit dependency covers this invocation's framebuffer sample.
    * Other samples and unmapped inputs retain their original texture read. */
   nir_def_rewrite_uses_after(&intr->def, result);
   return true;
}
