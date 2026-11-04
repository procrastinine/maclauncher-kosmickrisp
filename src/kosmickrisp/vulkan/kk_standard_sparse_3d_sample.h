/* SPDX-License-Identifier: MIT */
/* Standard volumes and standard 2D images share the sampler footprint
 * builder. Every contributing tap remains an ordinary/sparse fetch, so
 * descriptor lowering supplies the same canonical physical mapping and the
 * existing residency writer. */
struct kk_standard_3d_tap_context {
   const struct lower_descriptors_ctx *descriptors;
   nir_def *address, *unnormalized, *border;
};

static nir_def *
kk_standard_3d_texel_tap(nir_builder *b, nir_tex_instr *tex, nir_def *raw,
                         nir_def *layer, nir_def *size, nir_def *level,
                         struct kk_standard_3d_tap_context *ctx);

static nir_def *
kk_standard_3d_sample_tap(nir_builder *b, nir_tex_instr *tex, nir_def *center,
                          nir_def *level, void *data)
{
   struct kk_standard_3d_tap_context *ctx = data;
   unsigned dims = tex->sampler_dim == GLSL_SAMPLER_DIM_3D ? 3 : 2;
   nir_def *size = nir_build_texture_query(b, tex, nir_texop_txs,
      dims + tex->is_array, nir_type_uint32, false, false);
   nir_tex_instr_add_src(nir_instr_as_tex(nir_def_instr(size)), nir_tex_src_lod, level);
   nir_def *extent = nir_trim_vector(b, size, dims);
   nir_def *scaled = nir_bcsel(b, ctx->unnormalized, nir_trim_vector(b, center, dims),
                                nir_fmul(b, nir_trim_vector(b, center, dims), nir_u2f32(b, extent)));
   /* Array layers select clamp(RNE(layer)) as in ordinary sampling. */
   nir_def *layer = tex->is_array ? nir_imin(b, nir_imax(b,
      nir_f2i32(b, nir_fround_even(b, nir_channel(b, center, dims))), nir_imm_int(b, 0)),
      nir_iadd_imm(b, nir_channel(b, size, dims), -1)) : NULL;
   return kk_standard_3d_texel_tap(b, tex, nir_f2i32(b, nir_ffloor(b, scaled)),
                                   layer, extent, level, ctx);
}

/* One texel of the footprint after address-mode wrapping, or the border. */
static nir_def *
kk_standard_3d_texel_tap(nir_builder *b, nir_tex_instr *tex, nir_def *raw,
                         nir_def *layer, nir_def *size, nir_def *level,
                         struct kk_standard_3d_tap_context *ctx)
{
   unsigned dims = raw->num_components;
   nir_def *coord[4], *inside = nir_imm_true(b);
   for (unsigned axis = 0; axis < dims; axis++) {
      nir_def *p = nir_channel(b, raw, axis), *n = nir_channel(b, size, axis);
      nir_def *mode = nir_iand_imm(b, nir_ushr_imm(b, ctx->address, 3 * axis), 7);
      nir_def *repeat = nir_imod(b, p, n);
      nir_def *mirror = nir_imod(b, p, nir_imul_imm(b, n, 2));
      mirror = nir_bcsel(b, nir_ilt(b, mirror, n), mirror,
         nir_isub(b, nir_iadd_imm(b, nir_imul_imm(b, n, 2), -1), mirror));
      nir_def *mirror_once = nir_bcsel(b, nir_ilt_imm(b, p, 0),
         nir_iadd_imm(b, nir_ineg(b, p), -1), p);
      nir_def *clamp = nir_imin(b, nir_imax(b, p, nir_imm_int(b, 0)), nir_iadd_imm(b, n, -1));
      mirror_once = nir_imin(b, mirror_once, nir_iadd_imm(b, n, -1));
      coord[axis] = nir_bcsel(b, nir_ieq_imm(b, mode, VK_SAMPLER_ADDRESS_MODE_REPEAT), repeat,
         nir_bcsel(b, nir_ieq_imm(b, mode, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT), mirror,
            nir_bcsel(b, nir_ieq_imm(b, mode, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE),
               mirror_once, clamp)));
      inside = nir_iand(b, inside, nir_ior(b,
         nir_ine_imm(b, mode, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER), nir_ult(b, p, n)));
   }
   nir_push_if(b, inside);
   nir_tex_instr *tap = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &tap->instr);
   b->cursor = nir_before_instr(&tap->instr);
   for (int i = tap->num_srcs - 1; i >= 0; i--)
      if (tap->src[i].src_type != nir_tex_src_texture_deref &&
          tap->src[i].src_type != nir_tex_src_plane)
         nir_tex_instr_remove_src(tap, i);
   tap->op = nir_texop_txf;
   tap->is_shadow = false;
   tap->component = 0;
   memset(tap->tg4_offsets, 0, sizeof(tap->tg4_offsets));
   if (layer) coord[dims] = layer;
   nir_tex_instr_add_src(tap, nir_tex_src_coord, nir_vec(b, coord, dims + !!layer));
   nir_tex_instr_add_src(tap, nir_tex_src_lod, level);
   b->cursor = nir_after_instr(&tap->instr);
   nir_push_else(b, NULL);
   nir_def *border = nir_convert_to_bit_size(b, ctx->border,
      nir_alu_type_get_base_type(tex->dest_type), tex->def.bit_size);
   if (tex->is_sparse) border = nir_vector_insert_imm(b, nir_pad_vector(b, border, 5),
      nir_imm_int(b, 0), 4);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, &tap->def, border);
}

/* A gather reads the 2x2 bilinear footprint of one level; each texel is a
 * fetch tap, and component i comes from the corner Vulkan assigns to it. */
static nir_def *
kk_standard_2d_gather(nir_builder *b, nir_tex_instr *tex, nir_deref_instr *sampler,
                      unsigned offset, const struct lower_descriptors_ctx *ctx)
{
   nir_def *address = kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_address_modes), ctx);
   struct kk_standard_3d_tap_context taps = {
      .descriptors = ctx, .address = address, .unnormalized = nir_test_mask(b, address, 1u << 9),
      .border = load_resource_deref_desc(b, 4, 32, sampler,
         offset + offsetof(struct kk_sampled_image_descriptor, border), ctx),
   };
   nir_def *lod = nir_get_tex_src(tex, nir_tex_src_lod);
   nir_def *level = lod ? nir_f2u32(b, nir_fround_even(b, nir_f2f32(b, lod))) : nir_imm_int(b, 0);
   nir_def *size = nir_build_texture_query(b, tex, nir_texop_txs,
      2 + tex->is_array, nir_type_uint32, false, false);
   nir_tex_instr_add_src(nir_instr_as_tex(nir_def_instr(size)), nir_tex_src_lod, level);
   nir_def *extent = nir_trim_vector(b, size, 2);
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *uv = nir_trim_vector(b, coord, 2);
   nir_def *p = nir_fadd_imm(b, nir_bcsel(b, taps.unnormalized, uv,
      nir_fmul(b, uv, nir_u2f32(b, extent))), -0.5);
   nir_def *base = nir_f2i32(b, nir_ffloor(b, p));
   nir_def *constant = nir_get_tex_src(tex, nir_tex_src_offset);
   if (constant) base = nir_iadd(b, base, nir_i2i32(b, constant));
   nir_def *layer = tex->is_array ? nir_imin(b, nir_imax(b,
      nir_f2i32(b, nir_fround_even(b, nir_channel(b, coord, 2))), nir_imm_int(b, 0)),
      nir_iadd_imm(b, nir_channel(b, size, 2), -1)) : NULL;
   static const int corner[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
   bool explicit_offsets = nir_tex_instr_has_explicit_tg4_offsets(tex);
   nir_def *parts[5], *status = NULL;
   for (unsigned i = 0; i < 4; i++) {
      nir_def *at = nir_iadd(b, base, nir_imm_ivec2(b,
         corner[i][0] + (explicit_offsets ? tex->tg4_offsets[i][0] : 0),
         corner[i][1] + (explicit_offsets ? tex->tg4_offsets[i][1] : 0)));
      nir_def *texel = kk_standard_3d_texel_tap(b, tex, at, layer, extent, level, &taps);
      parts[i] = nir_channel(b, texel, tex->component);
      if (tex->is_sparse)
         status = status ? nir_ior(b, status, nir_channel(b, texel, 4)) : nir_channel(b, texel, 4);
   }
   if (tex->is_sparse) parts[4] = status;
   return nir_vec(b, parts, 4 + tex->is_sparse);
}

static bool
kk_standard_3d_sample(nir_builder *b, nir_tex_instr *tex, void *data)
{
   const struct lower_descriptors_ctx *ctx = data;
   bool three_d = tex->sampler_dim == GLSL_SAMPLER_DIM_3D && ctx->standard_3d;
   bool two_d = tex->sampler_dim == GLSL_SAMPLER_DIM_2D && ctx->standard_2d;
   bool gather = two_d && tex->op == nir_texop_tg4;
   if ((!three_d && !two_d) || tex->is_shadow ||
       (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
        tex->op != nir_texop_txl && tex->op != nir_texop_txd && !gather)) return false;
   nir_deref_instr *texture = nir_get_tex_deref(tex, nir_tex_src_texture_deref);
   nir_deref_instr *sampler = nir_get_tex_deref(tex, nir_tex_src_sampler_deref);
   if (!texture || !sampler) return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *plane = nir_get_tex_src(tex, nir_tex_src_plane);
   unsigned offset = plane ? nir_src_as_uint(nir_src_for_ssa(plane)) * sizeof(struct kk_sampled_image_descriptor) : 0;
   nir_def *query = kk_standard_3d_sample_query(b, texture, offset, ctx);
   nir_def *kind = kk_nir_query_record_kind(b, query);
   nir_def *standard = nir_ieq_imm(b, kind, three_d ? KK_QUERY_RECORD_KIND_STANDARD_3D :
                                                     KK_QUERY_RECORD_KIND_STANDARD_2D);
   if (gather) {
      nir_push_if(b, standard);
      nir_def *gathered = kk_standard_2d_gather(b, tex, sampler, offset, ctx);
      nir_push_else(b, NULL);
      nir_tex_instr *native = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
      nir_builder_instr_insert(b, &native->instr);
      nir_pop_if(b, NULL);
      nir_def_replace(&tex->def, nir_if_phi(b, gathered, &native->def));
      return true;
   }
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *dx = nir_get_tex_src(tex, nir_tex_src_ddx), *dy = nir_get_tex_src(tex, nir_tex_src_ddy);
   bool implicit = tex->op == nir_texop_tex || tex->op == nir_texop_txb;
   /* Derivatives exclude the array layer. */
   nir_def *spatial = tex->is_array ? nir_trim_vector(b, coord, coord->num_components - 1) : coord;
   if (implicit) { dx = nir_ddx(b, spatial); dy = nir_ddy(b, spatial); }
   nir_push_if(b, standard);
   nir_def *lod = kk_minmax_lod(b, tex, dx, dy);
   nir_def *minimum = nir_f2f32(b, kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_lod_min_fp16), ctx));
   nir_def *maximum = nir_f2f32(b, kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16), ctx));
   lod = nir_fmin(b, nir_fmax(b, lod, minimum), maximum);
   nir_def *image_min = nir_get_tex_src(tex, nir_tex_src_min_lod);
   if (image_min) lod = nir_fmax(b, lod, nir_f2f32(b, image_min));
   nir_def *filter = kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits), ctx);
   nir_def *linear = nir_ine_imm(b, nir_iand(b, filter,
      nir_bcsel(b, nir_fle_imm(b, lod, 0), nir_imm_intN_t(b, 2, 16), nir_imm_intN_t(b, 1, 16))), 0);
   nir_def *levels = nir_build_texture_query(b, tex, nir_texop_query_levels,
      1, nir_type_uint32, false, false);
   lod = nir_fclamp(b, lod, nir_imm_float(b, 0), nir_u2f32(b, nir_iadd_imm(b, levels, -1)));
   nir_def *mipmap = kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode), ctx);
   nir_def *mip_linear = nir_ieq_imm(b, mipmap, VK_SAMPLER_MIPMAP_MODE_LINEAR);
   nir_def *nearest = nir_fmax(b, nir_fadd_imm(b, nir_fceil(b, nir_fadd_imm(b, lod, 0.5)), -1), nir_imm_float(b, 0));
   nir_def *low = nir_f2u32(b, nir_bcsel(b, mip_linear, nir_ffloor(b, lod), nearest));
   nir_def *mode = kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_reduction_mode), ctx);
   nir_def *address = kk_minmax_metadata(b, tex, sampler,
      offset + offsetof(struct kk_sampled_image_descriptor, sampler_address_modes), ctx);
   struct kk_standard_3d_tap_context taps = {
      .descriptors = ctx, .address = address, .unnormalized = nir_test_mask(b, address, 1u << 9),
      .border = load_resource_deref_desc(b, 4, 32, sampler,
         offset + offsetof(struct kk_sampled_image_descriptor, border), ctx),
   };
   nir_def *result = kk_minmax_level(b, tex, low, linear, mode, taps.unnormalized,
      true, kk_standard_3d_sample_tap, &taps);
   nir_def *next = nir_iadd_imm(b, low, 1);
   nir_push_if(b, nir_iand(b, mip_linear, nir_iand(b, nir_flt(b, nir_u2f32(b, low), lod), nir_ult(b, next, levels))));
   nir_def *upper = kk_minmax_level(b, tex, next, linear, mode, taps.unnormalized,
      true, kk_standard_3d_sample_tap, &taps);
   nir_def *combined = kk_minmax_combine(b, result, upper, mode, tex->is_sparse, tex->dest_type);
   if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
      nir_def *a = tex->is_sparse ? nir_trim_vector(b, result, 4) : result;
      nir_def *c = tex->is_sparse ? nir_trim_vector(b, upper, 4) : upper;
      nir_def *mix = nir_flrp(b, a, c, nir_f2fN(b, nir_fsub(b, lod, nir_u2f32(b, low)), a->bit_size));
      if (tex->is_sparse) mix = nir_vector_insert_imm(b, nir_pad_vector(b, mix, 5), nir_channel(b, combined, 4), 4);
      combined = nir_bcsel(b, nir_ieq_imm(b, mode, 0), mix, combined);
   }
   nir_pop_if(b, NULL);
   result = nir_if_phi(b, combined, result);
   nir_push_else(b, NULL);
   nir_tex_instr *native = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &native->instr);
   b->cursor = nir_before_instr(&native->instr);
   if (implicit) {
      nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
      if (bias) { nir_def *scale = nir_fexp2(b, nir_f2f32(b, bias)); dx = nir_fmul(b, dx, scale); dy = nir_fmul(b, dy, scale); }
      int source = nir_tex_instr_src_index(native, nir_tex_src_bias);
      if (source >= 0) nir_tex_instr_remove_src(native, source);
      native->op = nir_texop_txd;
      nir_tex_instr_add_src(native, nir_tex_src_ddx, dx);
      nir_tex_instr_add_src(native, nir_tex_src_ddy, dy);
   }
   b->cursor = nir_after_instr(&native->instr);
   nir_pop_if(b, NULL);
   nir_def_replace(&tex->def, nir_if_phi(b, result, &native->def));
   return true;
}

bool
kk_nir_lower_standard_3d_samples(nir_shader *nir, uint32_t count,
   struct vk_descriptor_set_layout *const *layouts, bool three_d, bool two_d)
{
   struct lower_descriptors_ctx ctx = {.standard_3d = three_d, .standard_2d = two_d};
   for (unsigned i = 0; i < count; i++) if (layouts[i])
      ctx.set_layouts[i] = vk_to_kk_descriptor_set_layout(layouts[i]);
   return nir_shader_tex_pass(nir, kk_standard_3d_sample, nir_metadata_none, &ctx);
}
