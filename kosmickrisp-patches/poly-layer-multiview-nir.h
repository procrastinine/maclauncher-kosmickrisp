/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_LAYER_MULTIVIEW_NIR_H
#define KK_POLY_LAYER_MULTIVIEW_NIR_H

struct kk_poly_layer_capture_view_state {
   uint32_t view_mask;
};

static bool
kk_poly_layer_capture_amplification(nir_builder *b, nir_intrinsic_instr *intr,
                                    void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_amplification_id_kk)
      return false;
   const struct kk_poly_layer_capture_view_state *state = data;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *view = kk_poly_multiview_index(b);
   /* The upstream native lowering indexed a packed table of set view bits.
    * Select that same entry using the current original view, not its ordinal
    * as a framebuffer layer. Non-contiguous masks therefore retain ViewIndex. */
   nir_def *earlier = nir_iand_imm(b,
      nir_iadd_imm(b, nir_ishl(b, nir_imm_int(b, 1), view), -1), state->view_mask);
   nir_def_replace(&intr->def, nir_bit_count(b, earlier));
   return true;
}

/* Reuse the already-lowered native VS as the captured sibling AFTER cloning
 * its ordinary native alternative. No user body is retained or run twice.
 * As in pinned Honeykrisp, multiview supplies effective Layer=ViewIndex. */
static void
kk_poly_layer_capture_view(nir_shader *nir, uint32_t view_mask)
{
   assert(nir->info.stage == MESA_SHADER_VERTEX && view_mask);
   const struct kk_poly_layer_capture_view_state state = {view_mask};
   nir_shader_intrinsics_pass(nir, kk_poly_layer_capture_amplification,
                              nir_metadata_control_flow, (void *)&state);
   BITSET_CLEAR(nir->info.system_values_read, SYSTEM_VALUE_AMPLIFICATION_ID_KK);
   nir_builder b = nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));
   nir_store_output(&b, kk_poly_multiview_index(&b), nir_imm_int(&b, 0),
      .src_type = nir_type_uint32, .io_semantics.location = VARYING_SLOT_LAYER,
      .io_semantics.num_slots = 1);
   nir->info.outputs_written |= VARYING_BIT_LAYER;
}
#endif
