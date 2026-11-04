/* SPDX-License-Identifier: MIT
 * Sparse owns query identity/status plumbing; shared helpers own the one
 * filter/LOD/PCF/anisotropic footprint and its contributing sparse taps.
 */
#ifndef KK_SPARSE_SAMPLE_NIR_H
#define KK_SPARSE_SAMPLE_NIR_H
static nir_def *
kk_sparse_sample_field(nir_builder *b, nir_deref_instr *deref, unsigned plane,
                       unsigned offset, const struct lower_descriptors_ctx *ctx)
{
   return load_resource_deref_desc(b, 1, 16, deref,
      plane * sizeof(struct kk_sampled_image_descriptor) + offset, ctx);
}

static nir_def *
kk_sparse_native_sampler_list_address(nir_builder *b, nir_def *table, nir_def *source)
{
   nir_variable *cursor = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_sampler");
   nir_variable *result = nir_local_variable_create(b->impl, glsl_uint64_t_type(), "sparse_sampler_params");
   nir_store_var(b, cursor, nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, table, KK_SPARSE_NATIVE_SAMPLER_HEAD), .align_mul = 8), 1);
   nir_store_var(b, result, nir_imm_int64(b, 0), 1);
   nir_push_loop(b);
   nir_def *record = nir_load_var(b, cursor);
   nir_push_if(b, nir_ieq_imm(b, record, 0));
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);
   nir_def *id = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, offsetof(struct kk_sparse_native_sampler_record, source_id)), .align_mul = 8);
   nir_push_if(b, nir_ieq(b, id, source));
   nir_store_var(b, result,
      record, 1);
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);
   nir_store_var(b, cursor, nir_load_global_constant(b, 1, 64, record, .align_mul = 8), 1);
   nir_pop_loop(b, NULL);
   return nir_load_var(b, result);
}

/* Existing indexed rows also cover native heap sources with a real index.
 * Validate the complete opaque ID before using the row; native-only entries
 * without an index retain the existing full-ID list lookup. */
static nir_def *
kk_sparse_native_sampler_record_address(nir_builder *b, nir_def *table,
                                        nir_def *source, nir_def *index)
{
   nir_if *indexed = nir_push_if(b, nir_ult_imm(b, index, MSL_MAX_SAMPLERS));
   nir_def *row = nir_iadd(b, table,
      nir_imul_imm(b, nir_u2u64(b, index), KK_SPARSE_SAMPLER_STRIDE));
   nir_def *params = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, row, KK_SPARSE_ROW_NATIVE_PARAMS_WORD * sizeof(uint64_t)),
      .align_mul = 8);
   nir_if *present = nir_push_if(b, nir_ine_imm(b, params, 0));
   nir_def *record = nir_iadd_imm(b, params,
      -(int)offsetof(struct kk_sparse_native_sampler_record, params));
   nir_def *id = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, offsetof(struct kk_sparse_native_sampler_record, source_id)),
      .align_mul = 8);
   nir_def *match = nir_bcsel(b, nir_ieq(b, id, source), record, nir_imm_int64(b, 0));
   nir_push_else(b, present);
   nir_def *missing = nir_imm_int64(b, 0);
   nir_pop_if(b, present);
   match = nir_if_phi(b, match, missing);
   nir_push_else(b, indexed);
   nir_def *unindexed = nir_imm_int64(b, 0);
   nir_pop_if(b, indexed);
   record = nir_if_phi(b, match, unindexed);
   nir_if *fallback = nir_push_if(b, nir_ieq_imm(b, record, 0));
   nir_def *listed = kk_sparse_native_sampler_list_address(b, table, source);
   nir_push_else(b, fallback);
   nir_pop_if(b, fallback);
   return nir_if_phi(b, listed, record);
}

static bool
kk_sparse_sample_attach(nir_builder *b, nir_tex_instr *tex,
                         const struct lower_descriptors_ctx *ctx)
{
   nir_deref_instr *td = nir_get_tex_deref(tex, nir_tex_src_texture_deref);
   nir_deref_instr *sd = nir_get_tex_deref(tex, nir_tex_src_sampler_deref);
   if (!td || !sd) return false;
   nir_def *plane_src = nir_get_tex_src(tex, nir_tex_src_plane);
   unsigned plane = plane_src ? nir_src_as_uint(nir_src_for_ssa(plane_src)) : 0;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *tag = kk_sparse_sample_field(b, td, plane,
      offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16), ctx);
   nir_def *record = load_resource_deref_desc(b, 1, 64, td,
      plane * sizeof(struct kk_sampled_image_descriptor) +
      offsetof(struct kk_sampled_image_descriptor, aux_word), ctx);
   record = nir_bcsel(b, nir_test_mask(b, tag, KK_IMAGE_QUERY_RECORD_TAG), record,
                         nir_imm_int64(b, 0));
   /* Only a tagged, readable prefix may be probed. A future kind dispatcher
    * routes layered records first and preserves native kind0 for this pass. */
   nir_def *kind = kk_nir_query_record_kind(b, record);
   record = nir_bcsel(b, nir_ieq_imm(b, kind, KK_QUERY_RECORD_KIND_NATIVE), record,
                         nir_imm_int64(b, 0));
   nir_def *filters = kk_sparse_sample_field(b, sd, plane,
      offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits), ctx);
   nir_def *native = nir_inot(b, nir_test_mask(b, filters, KK_SAMPLER_FILTER_VIRTUAL));
   nir_if *lookup = nir_push_if(b, nir_ine_imm(b, record, 0));
   nir_def *table = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, offsetof(struct kk_sparse_query_record, sampler_table)), .align_mul = 8);
   nir_def *params, *facts;
   if (sd->deref_type == nir_deref_type_cast) {
      nir_if *native_lookup = nir_push_if(b, native);
      nir_def *source = load_resource_deref_desc(b, 1, 64, sd,
         plane * sizeof(struct kk_sampled_image_descriptor) +
         offsetof(struct kk_sampled_image_descriptor, aux_resource_id), ctx);
      nir_def *native_index = kk_sparse_sample_field(b, sd, plane,
         offsetof(struct kk_sampled_image_descriptor, sampler_index), ctx);
      nir_def *native_record = kk_sparse_native_sampler_record_address(b, table, source, native_index);
      nir_if *found = nir_push_if(b, nir_ine_imm(b, native_record, 0));
      nir_def *native_params = nir_iadd_imm(b, native_record,
         offsetof(struct kk_sparse_native_sampler_record, params));
      nir_def *native_facts = nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, native_record, offsetof(struct kk_sparse_native_sampler_record, physical_facts)),
         .align_mul = 4);
      nir_push_else(b, found);
      nir_def *missing_params = nir_imm_int64(b, 0);
      nir_def *missing_facts = nir_imm_int(b, 0);
      nir_pop_if(b, found);
      native_params = nir_if_phi(b, native_params, missing_params);
      native_facts = nir_if_phi(b, native_facts, missing_facts);
      nir_push_else(b, native_lookup);
      /* Virtual heap sources retain their pinned canonical index. Their
       * logical identity remains the descriptor's immutable params pointer. */
      nir_def *index = kk_sparse_sample_field(b, sd, plane,
         offsetof(struct kk_sampled_image_descriptor, sampler_index), ctx);
      nir_def *row = nir_iadd(b, table,
         nir_imul_imm(b, nir_u2u64(b, index), KK_SPARSE_SAMPLER_STRIDE));
      nir_def *virtual_facts = nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, row, KK_SPARSE_ROW_PHYSICAL_FACTS_WORD * sizeof(uint64_t)),
         .align_mul = 8);
      nir_def *virtual_params = nir_imm_int64(b, 0);
      nir_pop_if(b, native_lookup);
      params = nir_if_phi(b, native_params, virtual_params);
      facts = nir_if_phi(b, native_facts, virtual_facts);
   } else {
      nir_def *index = kk_sparse_sample_field(b, sd, plane,
         offsetof(struct kk_sampled_image_descriptor, sampler_index), ctx);
      nir_def *row = nir_iadd(b, table,
         nir_imul_imm(b, nir_u2u64(b, index), KK_SPARSE_SAMPLER_STRIDE));
      params = nir_load_global_constant(b, 1, 64,
         nir_iadd_imm(b, row, KK_SPARSE_ROW_NATIVE_PARAMS_WORD * sizeof(uint64_t)),
         .align_mul = 8);
      params = nir_bcsel(b, native, params, nir_imm_int64(b, 0));
      facts = nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, row, KK_SPARSE_ROW_PHYSICAL_FACTS_WORD * sizeof(uint64_t)),
         .align_mul = 8);
   }
   nir_push_else(b, lookup);
   nir_def *none = nir_imm_int64(b, 0), *no_facts = nir_imm_int(b, 0);
   nir_pop_if(b, lookup);
   params = nir_if_phi(b, params, none);
   facts = nir_if_phi(b, facts, no_facts);
   /* Descriptor lowering fills backend1.xy and preserves these exact bits. */
   assert(nir_tex_instr_src_index(tex, nir_tex_src_backend1) < 0);
   nir_tex_instr_add_src(tex, nir_tex_src_backend1, nir_vec4(b,
      nir_imm_int(b, 0), nir_imm_int(b, 0),
      nir_unpack_64_2x32_split_x(b, params), nir_unpack_64_2x32_split_y(b, params)));
   nir_def *lod = nir_get_tex_src(tex, nir_tex_src_backend2);
   if (lod) {
      int src = nir_tex_instr_src_index(tex, nir_tex_src_backend2);
      nir_src_rewrite(&tex->src[src].src, nir_vec4(b, nir_channel(b, lod, 0),
         nir_unpack_64_2x32_split_x(b, record), nir_unpack_64_2x32_split_y(b, record),
         facts));
   } else {
      assert(tex->backend_flags & KK_SAMPLE_SPARSE_POINT);
      nir_tex_instr_add_src(tex, nir_tex_src_backend2, nir_vec4(b, nir_imm_float(b, 0),
         nir_unpack_64_2x32_split_x(b, record), nir_unpack_64_2x32_split_y(b, record),
         facts));
   }
   tex->backend_flags |= KK_SAMPLE_SPARSE;
   return true;
}

static bool
kk_sparse_gather_samples(nir_builder *b, nir_tex_instr *tex,
                          const struct lower_descriptors_ctx *ctx)
{
   b->cursor = nir_before_instr(&tex->instr);
   bool cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
   nir_def *coord = nir_f2f32(b, nir_get_tex_src(tex, nir_tex_src_coord)), *face = NULL;
   nir_def *uv = cube ? nir_fmul_imm(b, kk_minmax_cube_uv(b, coord, &face), 0.5) :
                        nir_trim_vector(b, coord, 2);
   nir_def *size = nir_u2f32(b, nir_trim_vector(b, nir_get_texture_size(b, tex), 2));
   nir_def *base = nir_ffloor(b, nir_fadd_imm(b, nir_fmul(b, uv, size), -0.5));
   nir_def *offset = nir_get_tex_src(tex, nir_tex_src_offset);
   if (offset) base = nir_fadd(b, base, nir_i2f32(b, offset));
   static const unsigned corners[4][2] = {{0,1}, {1,1}, {1,0}, {0,0}};
   nir_def *result[5], *status = nir_imm_int(b, 0);
   for (unsigned i = 0; i < 4; ++i) {
      nir_def *center = nir_fdiv(b, nir_fadd(b, base, nir_vec2(b,
         nir_imm_float(b, corners[i][0] + 0.5), nir_imm_float(b, corners[i][1] + 0.5))), size);
      if (cube) center = kk_minmax_cube_direction(b, center, face);
      if (tex->is_array)
         center = nir_vector_insert_imm(b, nir_pad_vector(b, center, center->num_components + 1),
            nir_channel(b, coord, coord->num_components - 1), center->num_components);
      nir_tex_instr *tap = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
      nir_builder_instr_insert(b, &tap->instr);
      b->cursor = nir_before_instr(&tap->instr);
      for (int src = tap->num_srcs - 1; src >= 0; --src)
         if (tap->src[src].src_type == nir_tex_src_coord || tap->src[src].src_type == nir_tex_src_offset ||
             tap->src[src].src_type == nir_tex_src_lod || tap->src[src].src_type == nir_tex_src_min_lod)
            nir_tex_instr_remove_src(tap, src);
      tap->op = nir_texop_txl;
      tap->component = 0;
      tap->backend_flags &= ~(KK_SAMPLE_MINMAX | KK_SAMPLE_MIN | KK_SAMPLE_MAX);
      tap->backend_flags |= KK_SAMPLE_SPARSE | KK_SAMPLE_SPARSE_POINT;
      if (tap->is_shadow) { tap->is_new_style_shadow = true; tap->def.num_components = 2; }
      nir_tex_instr_add_src(tap, nir_tex_src_coord, center);
      nir_tex_instr_add_src(tap, nir_tex_src_lod, nir_imm_float(b, 0));
      if (!kk_sparse_sample_attach(b, tap, ctx)) UNREACHABLE("sparse gather sampler identity");
      b->cursor = nir_after_instr(&tap->instr);
      result[i] = nir_channel(b, &tap->def, tex->is_shadow ? 0 : tex->component);
      status = nir_ior(b, status, nir_channel(b, &tap->def, tap->def.num_components - 1));
   }
   result[4] = status;
   nir_def_replace(&tex->def, nir_vec(b, result, 5));
   return true;
}

bool
kk_nir_lower_sparse_sample_views(nir_shader *nir, uint32_t count,
                                 struct vk_descriptor_set_layout *const *layouts,
                                 UNUSED bool minmax)
{
   struct lower_descriptors_ctx ctx = {0};
   for (unsigned i = 0; i < count; ++i)
      if (layouts[i]) ctx.set_layouts[i] = vk_to_kk_descriptor_set_layout(layouts[i]);
   bool progress = false;
   struct util_dynarray work = UTIL_DYNARRAY_INIT;
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_tex) continue;
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      if (tex->is_sparse && (tex->op == nir_texop_tex || tex->op == nir_texop_txb ||
          tex->op == nir_texop_txl || tex->op == nir_texop_txd || tex->op == nir_texop_tg4))
         util_dynarray_append(&work, tex);
   }
   util_dynarray_foreach(&work, nir_tex_instr *, tex) {
      nir_builder b = nir_builder_at(nir_before_instr(&(*tex)->instr));
      bool changed = (*tex)->op == nir_texop_tg4 ? kk_sparse_gather_samples(&b, *tex, &ctx) :
                                                kk_sparse_sample_attach(&b, *tex, &ctx);
      if (changed) { nir_progress(true, b.impl, nir_metadata_none); progress = true; }
   }
   util_dynarray_fini(&work);
   return progress;
}
#endif
