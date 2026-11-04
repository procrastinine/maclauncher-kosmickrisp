/* SPDX-License-Identifier: MIT */
/* Mesa's runtime owns mapping/hash lowering. Convert its heap operations to
 * the same descriptor addresses used by the set path, preserving all metadata. */
struct kk_heap_nir_context {
   const uint64_t *embedded;
   uint32_t embedded_count;
   bool sparse_buffers;
};

static nir_def *
kk_heap_address(nir_builder *b, nir_def *offset, bool sampler)
{
   unsigned field = sampler ? kk_root_descriptor_offset(heap_sampler)
                            : kk_root_descriptor_offset(heap_resource);
   nir_def *base = load_root(b, 1, 64, nir_imm_int(b, field), 8);
   return nir_iadd(b, base, nir_u2u64(b, offset));
}

static nir_def *
kk_heap_resource_deref(nir_builder *b, nir_def *address,
                       const struct glsl_type *type)
{
   return &nir_build_deref_cast(b, address, nir_var_uniform, type, 0)->def;
}

static bool
kk_prepare_heap_instr(nir_builder *b, nir_instr *instr, void *data)
{
   const struct kk_heap_nir_context *ctx = data;
   b->cursor = nir_before_instr(instr);
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      bool progress = false;
      const struct glsl_type *type = glsl_sampler_type(tex->sampler_dim,
         tex->is_shadow, tex->is_array,
         nir_alu_type_get_base_type(tex->dest_type) == nir_type_int ? GLSL_TYPE_INT :
         nir_alu_type_get_base_type(tex->dest_type) == nir_type_uint ? GLSL_TYPE_UINT :
         GLSL_TYPE_FLOAT);
      for (unsigned i = 0; i < tex->num_srcs; ++i) {
         bool sampler = tex->src[i].src_type == nir_tex_src_sampler_heap_offset;
         if (!sampler && tex->src[i].src_type != nir_tex_src_texture_heap_offset)
            continue;
         nir_def *address = kk_heap_address(b, tex->src[i].src.ssa, sampler);
         if (sampler) address = nir_load_global_constant(b, 1, 64, address, .align_mul = 8);
         nir_src_rewrite(&tex->src[i].src, kk_heap_resource_deref(b, address, type));
         tex->src[i].src_type = sampler ? nir_tex_src_sampler_deref
                                       : nir_tex_src_texture_deref;
         progress = true;
      }
      if (tex->embedded_sampler) {
         assert(tex->sampler_index < ctx->embedded_count);
         nir_def *address = nir_imm_int64(b, ctx->embedded[tex->sampler_index]);
         nir_tex_instr_add_src(tex, nir_tex_src_sampler_deref,
                              kk_heap_resource_deref(b, address, type));
         tex->embedded_sampler = false;
         tex->sampler_index = 0;
         progress = true;
      }
      return progress;
   }
   if (instr->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   bool buffer = intr->intrinsic == nir_intrinsic_load_heap_descriptor &&
      (nir_intrinsic_resource_type(intr) & (nir_resource_type_uniform_buffer |
       nir_resource_type_read_only_storage_buffer | nir_resource_type_read_write_storage_buffer));
   /* Keep the descriptor identity until sparse IO consumes its record index. */
   if (buffer && ctx->sparse_buffers) return false;
   if (intr->intrinsic == nir_intrinsic_load_heap_descriptor ||
       intr->intrinsic == nir_intrinsic_load_resource_heap_data) {
      nir_def *address = kk_heap_address(b, intr->src[0].ssa, false);
      nir_def *value = nir_load_global_constant(b, intr->def.num_components,
         intr->def.bit_size, address,
         .align_mul = intr->intrinsic == nir_intrinsic_load_resource_heap_data ? nir_intrinsic_align_mul(intr) : 4,
         .align_offset = intr->intrinsic == nir_intrinsic_load_resource_heap_data ? nir_intrinsic_align_offset(intr) : 0);
      if (buffer) {
         assert(value->num_components == 4 && value->bit_size == 32);
         value = nir_vector_insert_imm(b, value, nir_imm_int(b, 0), 3);
      }
      nir_def_replace(&intr->def, value);
      return true;
   }
   if (!strncmp(nir_intrinsic_infos[intr->intrinsic].name, "image_heap_", 11)) {
      enum glsl_base_type base = GLSL_TYPE_UINT;
      if (nir_intrinsic_has_dest_type(intr) &&
          nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) == nir_type_float)
         base = GLSL_TYPE_FLOAT;
      const struct glsl_type *type = glsl_image_type(nir_intrinsic_image_dim(intr),
         nir_intrinsic_image_array(intr), base);
      nir_def *address = kk_heap_address(b, intr->src[0].ssa, false);
      switch (intr->intrinsic) {
#define KK_HEAP_IMAGE(OP) case nir_intrinsic_image_heap_##OP: intr->intrinsic = nir_intrinsic_image_deref_##OP; break
         KK_HEAP_IMAGE(load); KK_HEAP_IMAGE(sparse_load); KK_HEAP_IMAGE(store);
         KK_HEAP_IMAGE(atomic); KK_HEAP_IMAGE(atomic_swap); KK_HEAP_IMAGE(size);
         KK_HEAP_IMAGE(samples); KK_HEAP_IMAGE(levels); KK_HEAP_IMAGE(input_attachment_load);
         KK_HEAP_IMAGE(depth_input_attachment_load); KK_HEAP_IMAGE(stencil_input_attachment_load);
         KK_HEAP_IMAGE(load_raw_intel); KK_HEAP_IMAGE(store_raw_intel);
         KK_HEAP_IMAGE(fragment_mask_load_amd); KK_HEAP_IMAGE(store_block_agx);
#undef KK_HEAP_IMAGE
         default: UNREACHABLE("Unknown heap image intrinsic");
      }
      nir_src_rewrite(&intr->src[0], kk_heap_resource_deref(b, address, type));
      return true;
   }
   return false;
}

bool
kk_nir_prepare_descriptor_heaps(nir_shader *nir, uint32_t embedded_count,
                                 const uint64_t *embedded, bool sparse_buffers)
{
   const struct kk_heap_nir_context ctx = {embedded, embedded_count, sparse_buffers};
   return nir_shader_instructions_pass(nir, kk_prepare_heap_instr,
                                       nir_metadata_control_flow, (void *)&ctx);
}

bool
kk_nir_finish_sparse_heap_buffers(nir_shader *nir)
{
   const struct kk_heap_nir_context ctx = {0};
   return nir_shader_instructions_pass(nir, kk_prepare_heap_instr,
                                       nir_metadata_control_flow, (void *)&ctx);
}
