/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_LAYER_REPLAY_NIR_H
#define KK_POLY_LAYER_REPLAY_NIR_H

static bool
kk_poly_layer_alias_possible(const struct vk_graphics_pipeline_state *state,
                              const struct vk_features *features)
{
   /* Unused attachments may be absent from the pipeline formats. Incomplete
    * pipeline libraries likewise cannot rule out an attachment alias. */
   if (features->dynamicRenderingUnusedAttachments || !state || !state->rp ||
       !vk_render_pass_state_has_attachment_info(state->rp))
      return true;

   /* The shared image runtime permits uncompressed views of compressed
    * blocks only in the 64- and 128-bit format classes. Depth/stencil and
    * other complete known color formats cannot select this replay path. */
   for (unsigned i = 0; i < state->rp->color_attachment_count; ++i) {
      VkFormat format = state->rp->color_attachment_formats[i];
      if (format == VK_FORMAT_UNDEFINED || vk_format_is_compressed(format) ||
          vk_format_aspects(format) != VK_IMAGE_ASPECT_COLOR_BIT)
         continue;
      unsigned bytes = vk_format_get_blocksize(format);
      if (bytes == 8 || bytes == 16)
         return true;
   }
   return false;
}

static nir_def *
kk_poly_layer_selector(nir_builder *b)
{
   nir_def *address = nir_load_buffer_ptr_kk(b, 1, 64,
      .binding = KK_ARGUMENT_POLY_LAYER_REPLAY);
   nir_push_if(b, nir_ine_imm(b, address, 0));
   nir_def *selector = nir_load_global_constant(b, 2, 32, address, .align_mul = 4);
   nir_push_else(b, NULL);
   nir_def *disabled = nir_imm_ivec2(b, 0, 0);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, selector, disabled);
}

/* This pass consumes generated readers, never the original user VS/GS body.
 * Those readers have one final store per output, after producer-side stores
 * and EmitVertex selections have already been captured.
 * All raster vertices of a valid Vulkan primitive carry the same Layer.
 * Cached Poly polygon packets already select integer/provoking attributes.
 * NaN rejection follows Poly's existing rasterization-stream rejection. */
static void
kk_poly_layer_lower_vs(nir_shader *nir)
{
   nir_intrinsic_instr *position = NULL, *layer = NULL;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_output) continue;
            unsigned slot = nir_intrinsic_io_semantics(intr).location;
            if (slot == VARYING_SLOT_LAYER) { assert(!layer); layer = intr; }
            if (slot == VARYING_SLOT_POS) { assert(!position); position = intr; }
         }
      }
   }
   assert(position);
   if (!layer) {
      nir_builder zero = nir_builder_at(nir_after_instr(&position->instr));
      nir_store_output(&zero, nir_imm_int(&zero, 0), nir_imm_int(&zero, 0),
         .src_type = nir_type_uint32, .io_semantics.location = VARYING_SLOT_LAYER,
         .io_semantics.num_slots = 1);
      layer = nir_instr_as_intrinsic(zero.cursor.instr);
      nir->info.outputs_written |= VARYING_BIT_LAYER;
   }
   assert(position->instr.block == layer->instr.block);
   nir_intrinsic_instr *last = nir_instr_is_before(&layer->instr, &position->instr) ?
      position : layer;
   nir_builder b = nir_builder_at(nir_after_instr(&last->instr));
   nir_def *selector = kk_poly_layer_selector(&b);
   nir_def *enabled = nir_ine_imm(&b, nir_channel(&b, selector, 1), 0);
   nir_def *accept = nir_ieq(&b, nir_channel(&b, layer->src[0].ssa, 0),
                             nir_channel(&b, selector, 0));
   nir_def *original_position = position->src[0].ssa;
   nir_def *rejected = nir_vector_insert_imm(&b, original_position,
                                             nir_imm_float(&b, NAN), 3);
   nir_store_output(&b, nir_bcsel(&b, nir_ior(&b, nir_inot(&b, enabled), accept),
      original_position, rejected), nir_imm_int(&b, 0),
      .src_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_POS,
      .io_semantics.num_slots = 1);
   nir_store_output(&b, nir_bcsel(&b, enabled, nir_imm_int(&b, 0),
      nir_channel(&b, layer->src[0].ssa, 0)), nir_imm_int(&b, 0),
      .src_type = nir_type_uint32, .io_semantics.location = VARYING_SLOT_LAYER,
      .io_semantics.num_slots = 1);
   nir_instr_remove(&position->instr);
   nir_instr_remove(&layer->instr);
   nir_progress(true, b.impl, nir_metadata_none);
}

static bool
kk_poly_layer_fs_intrinsic(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_layer_id) return false;
   b->cursor = nir_after_instr(&intr->instr);
   nir_def *selector = kk_poly_layer_selector(b);
   nir_def *value = nir_bcsel(b,
      nir_ine_imm(b, nir_channel(b, selector, 1), 0),
      nir_channel(b, selector, 0), &intr->def);
   /* The null guard splits blocks, so the same-block rewrite-after helper
    * cannot distinguish our bcsel's original-value source. */
   nir_foreach_use_including_if_safe(use, &intr->def) {
      if (!nir_src_is_if(use) && nir_src_use_instr(use) == nir_def_instr(value))
         continue;
      nir_src_rewrite(use, value);
   }
   return true;
}

static void
kk_poly_layer_lower_fs(nir_shader *nir)
{
   nir_shader_intrinsics_pass(nir, kk_poly_layer_fs_intrinsic,
                              nir_metadata_none, NULL);
}
#endif
