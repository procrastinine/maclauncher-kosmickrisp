/*
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 *
 * Per-view replay follows Honeykrisp hk_lower_multiview/hk_foreach_view in
 * Mesa 4f071f891d63396c799b267172c19ba6b0301682. Native vertex amplification
 * remains the ordinary VS path; Poly preraster stages read the replay index.
 */

static nir_def *
kk_poly_multiview_index(nir_builder *b)
{
   nir_def *base = nir_load_per_draw_ptr_kk(b, 1, 64);
   return nir_build_load_global_constant(
      b, 1, 32,
      nir_iadd_imm(b, base,
         offsetof(struct kk_per_draw_data, multiview_index)), .align_mul = 4);
}

struct kk_poly_multiview_lower {
   nir_variable *layer;
   bool enabled;
};

static bool
kk_poly_multiview_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                            void *data)
{
   struct kk_poly_multiview_lower *state = data;
   nir_variable *layer = state->layer;
   if (intr->intrinsic == nir_intrinsic_load_view_index) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_def_replace(&intr->def, state->enabled ?
         kk_poly_multiview_index(b) : nir_imm_int(b, 0));
      return true;
   }

   /* A GS's output values become undefined after EmitVertex. Write the layer
    * for every emitted vertex, independently of the application's outputs. */
   if (layer && (intr->intrinsic == nir_intrinsic_emit_vertex ||
                 intr->intrinsic == nir_intrinsic_emit_vertex_with_counter)) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_store_var(b, layer, kk_poly_multiview_index(b), 1);
      return true;
   }
   return false;
}

static bool
kk_nir_lower_poly_multiview(nir_shader *nir, bool enabled,
                             bool last_preraster)
{
   bool last = enabled && last_preraster;
   nir_variable *layer = NULL;
   if (last) {
      layer = nir_find_variable_with_location(
         nir, nir_var_shader_out, VARYING_SLOT_LAYER);
      if (!layer)
         layer = nir_create_variable_with_location(
            nir, nir_var_shader_out, VARYING_SLOT_LAYER, glsl_uint_type());
      nir->info.outputs_written |= VARYING_BIT_LAYER;
   }

   struct kk_poly_multiview_lower state = {
      .layer = nir->info.stage == MESA_SHADER_GEOMETRY ? layer : NULL,
      .enabled = enabled,
   };
   bool progress = nir_shader_intrinsics_pass(
      nir, kk_poly_multiview_intrinsic, nir_metadata_control_flow,
      &state);

   if (layer && nir->info.stage != MESA_SHADER_GEOMETRY) {
      nir_builder b = nir_builder_at(
         nir_after_impl(nir_shader_get_entrypoint(nir)));
      nir_store_var(&b, layer, kk_poly_multiview_index(&b), 1);
      progress = true;
   }
   return progress;
}
