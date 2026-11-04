/* SPDX-License-Identifier: MIT */
/* Cube footprints use the existing cube projection/gradient utilities and
 * ordinary typed layer descriptors. No opaque native ID is an address. */
static nir_def *
kk_layered_bc_cube_point(nir_builder *b, nir_tex_instr *tex,
                         nir_def *table, nir_def *cube, nir_def *face,
                         nir_def *pixel, nir_def *size, nir_def *seamless,
                         nir_def *remap)
{
   nir_def *center = nir_fdiv(b, nir_fadd_imm(b, pixel, 0.5), size);
   nir_def *adjacent;
   nir_def *uv = nir_fmul_imm(b, kk_minmax_cube_uv(b,
      kk_minmax_cube_direction(b, center, face), &adjacent), 0.5);
   nir_def *mapped = nir_ffloor(b, nir_fmul(b, uv, size));
   pixel = nir_bcsel(b, remap, mapped, pixel);
   face = nir_bcsel(b, remap, adjacent, face);
   /* Seamless nearest ignores addressing and clamps to the selected face.
    * Linear/gather taps have already selected the incident adjacent face. */
   pixel = nir_bcsel(b, seamless, nir_fmin(b, nir_fmax(b, pixel,
      nir_imm_float(b, 0)), nir_fadd_imm(b, size, -1)), pixel);
   center = nir_fdiv(b, nir_fadd_imm(b, pixel, 0.5), size);
   nir_def *index = nir_iadd(b, nir_imul_imm(b, cube, 6), face);
   nir_def *address = nir_iadd(b, table, nir_imul_imm(b, nir_u2u64(b, index),
      sizeof(struct kk_sampled_image_descriptor)));
   nir_deref_instr *original = nir_get_tex_deref(tex, nir_tex_src_texture_deref);
   const struct glsl_type *type = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false,
      true, glsl_get_sampler_result_type(original->type));
   nir_deref_instr *selected = nir_build_deref_cast(b, address,
      nir_var_uniform, type, 0);
   nir_def *tap_coord = nir_vec3(b, nir_channel(b, center, 0),
      nir_channel(b, center, 1), nir_imm_float(b, 0));
   nir_def *tap_lod = nir_imm_float(b, 0);
   nir_tex_instr *tap = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &tap->instr);
   for (int i = tap->num_srcs - 1; i >= 0; --i)
      if (tap->src[i].src_type != nir_tex_src_sampler_deref)
         nir_tex_instr_remove_src(tap, i);
   tap->op = nir_texop_txl;
   tap->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tap->is_array = true;
   tap->coord_components = 3;
   tap->component = 0;
   tap->backend_flags = 0;
   /* Native filtering at a texel center is one contributing texel. Keeping
    * the original sampler delegates addressing/custom border to the shared
    * sampling path, including its existing sparse status writer. */
   nir_tex_instr_add_src(tap, nir_tex_src_texture_deref, &selected->def);
   nir_tex_instr_add_src(tap, nir_tex_src_coord, tap_coord);
   nir_tex_instr_add_src(tap, nir_tex_src_lod, tap_lod);
   return &tap->def;
}

static nir_def *
kk_layered_bc_cube_lod(nir_builder *b, nir_tex_instr *tex, nir_def *coord,
                       nir_def *face, nir_def *size, nir_def *dx, nir_def *dy)
{
   nir_def *lod = nir_get_tex_src(tex, nir_tex_src_lod);
   if (lod) return nir_f2f32(b, lod);
   nir_def *gx = nir_fmul(b, kk_cube_face_gradient(b, coord, dx, face), size);
   nir_def *gy = nir_fmul(b, kk_cube_face_gradient(b, coord, dy, face), size);
   nir_def *rho = nir_fmax(b, nir_fdot(b, gx, gx), nir_fdot(b, gy, gy));
   lod = nir_fmul_imm(b, nir_flog2(b, rho), 0.5);
   nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
   if (bias) lod = nir_fadd(b, lod, nir_f2f32(b, bias));
   nir_def *sampler_bias = kk_sampler_query_metadata(b, tex,
      KK_SAMPLER_META_QUERY_BIAS, false);
   return nir_fadd(b, lod, sampler_bias);
}

static nir_def *
kk_layered_bc_cube_combine(nir_builder *b, nir_def *a, nir_def *tap,
                           nir_def *weight, nir_def *mode,
                           nir_tex_instr *tex)
{
   nir_def *v = tex->is_sparse ? nir_trim_vector(b, tap, 4) : tap;
   nir_def *prior = tex->is_sparse ? nir_trim_vector(b, a, 4) : a;
   nir_def *sum = nir_fadd(b, prior,
      nir_fmul(b, v, nir_f2fN(b, weight, tex->def.bit_size)));
   if (tex->is_sparse)
      sum = nir_vector_insert_imm(b, nir_pad_vector(b, sum, 5),
         nir_ior(b, nir_channel(b, a, 4), nir_channel(b, tap, 4)), 4);
   /* Integer image filtering is nearest unless reduction filtering applies. */
   if (nir_alu_type_get_base_type(tex->dest_type) != nir_type_float)
      sum = tap;
   nir_def *reduced = kk_minmax_combine(b, a, tap, mode,
                                       tex->is_sparse, tex->dest_type);
   return nir_bcsel(b, nir_ine_imm(b, mode, 0), reduced, sum);
}

static nir_def *
kk_layered_bc_cube_filter(nir_builder *b, nir_tex_instr *tex, nir_def *table,
                           nir_def *cube, nir_def *face, nir_def *uv,
                           nir_def *size, nir_def *seamless, nir_def *linear,
                           nir_def *mode)
{
   bool gather = tex->op == nir_texop_tg4;
   nir_def *p = nir_fsub(b, nir_fmul(b, uv, size),
      nir_bcsel(b, linear, nir_imm_float(b, 0.5), nir_imm_float(b, 0)));
   nir_def *base = nir_ffloor(b, p);
   nir_def *fraction = nir_bcsel(b, linear, nir_fsub(b, p, base), nir_imm_vec2(b, 0, 0));
   nir_def *values[4], *status = nir_imm_int(b, 0);
   nir_def *result = nir_imm_zero(b, tex->def.num_components, tex->def.bit_size);
   nir_def *have = nir_imm_bool(b, false);
   for (unsigned tap = 0; tap < 4; ++tap) {
      /* Gather component order: (0,1),(1,1),(1,0),(0,0). */
      unsigned x = gather ? (tap == 1 || tap == 2) : tap & 1;
      unsigned y = gather ? (tap < 2) : tap >> 1;
      nir_def *at = nir_fadd(b, base, nir_imm_vec2(b, x, y));
      nir_def *offset = nir_get_tex_src(tex, nir_tex_src_offset);
      if (offset) at = nir_fadd(b, at, nir_i2f32(b, offset));
      if (gather && nir_tex_instr_has_explicit_tg4_offsets(tex))
         at = nir_fadd(b, at, nir_imm_vec2(b, tex->tg4_offsets[tap][0], tex->tg4_offsets[tap][1]));
      nir_def *weight = nir_fmul(b,
         x ? nir_channel(b, fraction, 0) : nir_fsub(b, nir_imm_float(b, 1), nir_channel(b, fraction, 0)),
         y ? nir_channel(b, fraction, 1) : nir_fsub(b, nir_imm_float(b, 1), nir_channel(b, fraction, 1)));
      if (!gather) nir_push_if(b, nir_fgt_imm(b, weight, 0));
      nir_def *value = kk_layered_bc_cube_point(b, tex, table, cube, face,
         at, size, seamless, nir_iand(b, seamless, linear));
      if (gather) {
         values[tap] = nir_channel(b, value, tex->component);
         if (tex->is_sparse) status = nir_ior(b, status, nir_channel(b, value, 4));
      } else {
         nir_def *combined = kk_layered_bc_cube_combine(b, result, value, weight, mode, tex);
         nir_def *first = value;
         if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
            first = nir_fmul(b, tex->is_sparse ? nir_trim_vector(b, value, 4) : value,
               nir_f2fN(b, weight, tex->def.bit_size));
            if (tex->is_sparse)
               first = nir_vector_insert_imm(b, nir_pad_vector(b, first, 5),
                                             nir_channel(b, value, 4), 4);
            first = nir_bcsel(b, nir_ine_imm(b, mode, 0), value, first);
         }
         nir_def *next = nir_bcsel(b, have, combined, first);
         nir_pop_if(b, NULL);
         result = nir_if_phi(b, next, result);
         have = nir_ior(b, have, nir_fgt_imm(b, weight, 0));
      }
   }
   if (gather) {
      result = nir_vec(b, values, 4);
      if (tex->is_sparse) result = nir_vector_insert_imm(b, nir_pad_vector(b, result, 5), status, 4);
   }
   return result;
}

/* The line footprint uses the actual original maximum from the common
 * sampler service. No native sampler ID or pointer word is decoded here.
 * A dynamic loop has exactly ceil(major / effective_minor) samples, bounded
 * by that legal original maximum rather than a private tap cutoff. */
static nir_def *
kk_layered_bc_cube_footprint(nir_builder *b, nir_tex_instr *tex,
                              nir_def *table, nir_def *cube, nir_def *face,
                              nir_def *uv, nir_def *size, nir_def *seamless,
                              nir_def *linear, nir_def *mode, nir_def *coord,
                              nir_def *dx, nir_def *dy)
{
   /* Gather and explicit scalar LOD have no anisotropic derivative footprint. */
   if (tex->op == nir_texop_tg4 || !dx || !dy)
      return kk_layered_bc_cube_filter(b, tex, table, cube, face, uv, size,
                                      seamless, linear, mode);
   nir_def *maximum = kk_sampler_query_metadata(b, tex,
      KK_SAMPLER_META_MAX_ANISOTROPY, false);
   /* A disabled/isotropic sampler has exactly one centered footprint.
    * Keep the common filter/loop below, but do not evaluate ellipse axes or
    * fetch the footprint-only bias when that result cannot affect the taps. */
   nir_push_if(b, nir_fgt_imm(b, maximum, 1));
   nir_def *gx = nir_fmul(b, kk_cube_face_gradient(b, coord, dx, face), size);
   nir_def *gy = nir_fmul(b, kk_cube_face_gradient(b, coord, dy, face), size);
   nir_def *bias = kk_sampler_query_metadata(b, tex,
      KK_SAMPLER_META_QUERY_BIAS, false);
   nir_def *explicit_bias = nir_get_tex_src(tex, nir_tex_src_bias);
   if (explicit_bias) bias = nir_fadd(b, bias, nir_f2f32(b, explicit_bias));
   nir_def *scale = nir_fexp2(b, bias);
   gx = nir_fmul(b, gx, scale);
   gy = nir_fmul(b, gy, scale);
   /* Principal axes of the projected ellipse, including parallel or rotated
    * screen derivatives. Merely choosing the longer derivative loses those
    * narrow footprints. */
   nir_def *x = nir_vec2(b, nir_channel(b, gx, 0), nir_channel(b, gy, 0));
   nir_def *y = nir_vec2(b, nir_channel(b, gx, 1), nir_channel(b, gy, 1));
   nir_def *xx = nir_fdot(b, x, x), *yy = nir_fdot(b, y, y);
   nir_def *xy = nir_fdot(b, x, y), *delta = nir_fsub(b, xx, yy);
   nir_def *discriminant = nir_fsqrt(b, nir_fadd(b, nir_fmul(b, delta, delta),
      nir_fmul_imm(b, nir_fmul(b, xy, xy), 4)));
   nir_def *trace = nir_fadd(b, xx, yy);
   nir_def *major_squared = nir_fmul_imm(b, nir_fadd(b, trace, discriminant), 0.5);
   nir_def *minor_squared = nir_fmax(b, nir_imm_float(b, 0),
      nir_fmul_imm(b, nir_fsub(b, trace, discriminant), 0.5));
   nir_def *major_length = nir_fsqrt(b, major_squared);
   nir_def *minor_length = nir_fsqrt(b, minor_squared);
   nir_def *axis = nir_bcsel(b, nir_fge(b, xx, yy),
      nir_vec2(b, nir_fsub(b, major_squared, yy), xy),
      nir_vec2(b, xy, nir_fsub(b, major_squared, xx)));
   nir_def *axis_squared = nir_fdot(b, axis, axis);
   nir_def *major = nir_bcsel(b, nir_fgt_imm(b, axis_squared, 0),
      nir_fmul(b, axis, nir_fmul(b, major_length,
         nir_frsq(b, nir_fmax(b, axis_squared, nir_imm_float(b, 0x1p-126))))),
      nir_vec2(b, major_length, nir_imm_float(b, 0)));
   nir_def *minor = nir_fmax(b, nir_imm_float(b, 1),
      nir_fmax(b, minor_length, nir_fdiv(b, major_length, maximum)));
   nir_def *ratio = nir_fmax(b, nir_imm_float(b, 1),
      nir_fmin(b, maximum, nir_fdiv(b, major_length, minor)));
   nir_def *samples = nir_umax(b, nir_imm_int(b, 1),
      nir_umin(b, nir_f2u32(b, nir_fceil(b, ratio)),
                    nir_f2u32(b, nir_fceil(b, maximum))));
   /* Zero offset for the isotropic/disabled case retains its exact center. */
   major = nir_bcsel(b, nir_ugt_imm(b, samples, 1), major,
                     nir_imm_vec2(b, 0, 0));
   nir_push_else(b, NULL);
   nir_def *center = nir_imm_vec2(b, 0, 0);
   nir_def *single = nir_imm_int(b, 1);
   nir_pop_if(b, NULL);
   major = nir_if_phi(b, major, center);
   samples = nir_if_phi(b, samples, single);
   nir_def *weight = nir_frcp(b, nir_u2f32(b, samples));
   nir_def *zero_index = nir_imm_int(b, 0);
   nir_def *zero_value = nir_imm_zero(b, tex->def.num_components,
                                    tex->def.bit_size);
   nir_block *before = nir_cursor_current_block(b->cursor);
   nir_loop *loop = nir_push_loop(b);
   nir_phi_instr *index = nir_phi_instr_create(b->shader);
   nir_def_init(&index->instr, &index->def, 1, 32);
   nir_phi_instr_add_src(index, before, zero_index);
   nir_builder_instr_insert(b, &index->instr);
   nir_phi_instr *value = nir_phi_instr_create(b->shader);
   nir_def_init(&value->instr, &value->def, tex->def.num_components,
                tex->def.bit_size);
   nir_phi_instr_add_src(value, before, zero_value);
   nir_builder_instr_insert(b, &value->instr);
   nir_push_if(b, nir_uge(b, &index->def, samples));
   nir_jump(b, nir_jump_break);
   nir_pop_if(b, NULL);
   nir_def *position = nir_fadd_imm(b, nir_fmul(b,
      nir_fadd_imm(b, nir_u2f32(b, &index->def), 0.5), weight), -0.5);
   nir_def *at = nir_fadd(b, uv, nir_fdiv(b, nir_fmul(b, major, position), size));
   nir_def *tap = kk_layered_bc_cube_filter(b, tex, table, cube, face,
                                           at, size, seamless, linear, mode);
   nir_def *first = tap;
   if (nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
      first = nir_fmul(b, tex->is_sparse ? nir_trim_vector(b, tap, 4) : tap,
                      nir_f2fN(b, weight, tex->def.bit_size));
      if (tex->is_sparse)
         first = nir_vector_insert_imm(b, nir_pad_vector(b, first, 5),
                                       nir_channel(b, tap, 4), 4);
      first = nir_bcsel(b, nir_ine_imm(b, mode, 0), tap, first);
   }
   nir_def *next_value = nir_bcsel(b, nir_ieq_imm(b, &index->def, 0), first,
      kk_layered_bc_cube_combine(b, &value->def, tap, weight, mode, tex));
   nir_def *next_index = nir_iadd_imm(b, &index->def, 1);
   nir_block *backedge = nir_cursor_current_block(b->cursor);
   /* These edges are added to already-inserted phis: initialize their SSA
    * use links explicitly, as required by the common NIR builder contract. */
   nir_phi_src *index_back = nir_phi_instr_add_src(index, backedge, next_index);
   nir_instr_init_src(&index->instr, &index_back->src, next_index);
   nir_phi_src *value_back = nir_phi_instr_add_src(value, backedge, next_value);
   nir_instr_init_src(&value->instr, &value_back->src, next_value);
   nir_pop_loop(b, loop);
   return &value->def;
}

static bool
kk_layered_bc_cube(nir_builder *b, nir_tex_instr *tex, void *data)
{
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE || tex->is_shadow)
      return false;
   bool query = tex->op == nir_texop_txs || tex->op == nir_texop_query_levels;
   if (!query && tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
       tex->op != nir_texop_txl && tex->op != nir_texop_txd &&
       tex->op != nir_texop_tg4 && tex->op != nir_texop_lod)
      return false;
   nir_deref_instr *texture = nir_get_tex_deref(tex, nir_tex_src_texture_deref);
   nir_deref_instr *sampler = nir_get_tex_deref(tex, nir_tex_src_sampler_deref);
   if (!texture || (!query && !sampler)) return false;
   const struct lower_descriptors_ctx *ctx = data;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *dx = nir_get_tex_src(tex, nir_tex_src_ddx);
   nir_def *dy = nir_get_tex_src(tex, nir_tex_src_ddy);
   bool implicit = tex->op == nir_texop_tex || tex->op == nir_texop_txb ||
                   tex->op == nir_texop_lod;
   /* Take quad derivatives before the descriptor-kind branch. */
   if (implicit) {
      dx = nir_ddx(b, nir_trim_vector(b, coord, 3));
      dy = nir_ddy(b, nir_trim_vector(b, coord, 3));
   }
   nir_def *flags = load_resource_deref_desc(b, 1, 16, texture,
      offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16), ctx);
   nir_push_if(b, nir_test_mask(b, flags, KK_IMAGE_QUERY_RECORD_TAG));
   nir_def *pointer = load_resource_deref_desc(b, 1, 64, texture,
      offsetof(struct kk_sampled_image_descriptor, aux_word), ctx);
   nir_push_else(b, NULL);
   nir_def *zero_pointer = nir_imm_int64(b, 0);
   nir_pop_if(b, NULL);
   nir_def *record = nir_if_phi(b, pointer, zero_pointer);
   nir_def *kind = kk_nir_query_record_kind(b, record);
   nir_push_if(b, nir_ieq_imm(b, kind, KK_QUERY_RECORD_KIND_LAYERED_BC));
   nir_def *dimensions = nir_load_global_constant(b, 3, 32,
      nir_iadd_imm(b, record, offsetof(struct kk_layered_bc_record, query.view_size)), .align_mul = 4);
   nir_def *count = nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, record, offsetof(struct kk_layered_bc_record, layer_count)), .align_mul = 4);
   nir_def *table = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, record, offsetof(struct kk_layered_bc_record, sampled_descriptors)), .align_mul = 8);
   nir_def *valid = nir_iand(b, nir_ine_imm(b, table, 0), nir_iand(b,
      nir_iand(b, nir_uge_imm(b, count, 6), nir_ieq_imm(b, nir_umod_imm(b, count, 6), 0)),
      nir_iand(b, nir_ine_imm(b, nir_channel(b, dimensions, 0), 0),
                  nir_ine_imm(b, nir_channel(b, dimensions, 1), 0))));
   nir_push_if(b, valid);
   nir_def *cube_count = nir_udiv_imm(b, count, 6);
   nir_def *result;
   if (query) {
      result = tex->op == nir_texop_query_levels ? nir_imm_int(b, 1) :
         tex->is_array ? nir_vec3(b, nir_channel(b, dimensions, 0),
            nir_channel(b, dimensions, 1), cube_count) : nir_trim_vector(b, dimensions, 2);
   } else {
      nir_def *face;
      nir_def *uv = nir_fmul_imm(b, kk_minmax_cube_uv(b, coord, &face), 0.5);
      nir_def *size = nir_u2f32(b, nir_trim_vector(b, dimensions, 2));
      nir_def *cube = nir_imm_int(b, 0);
      if (tex->is_array) {
         nir_def *layer = nir_ffloor(b, nir_fadd_imm(b, nir_channel(b, coord, 3), 0.5));
         layer = nir_fmin(b, nir_fmax(b, layer, nir_imm_float(b, 0)),
                          nir_u2f32(b, nir_iadd_imm(b, cube_count, -1)));
         cube = nir_umin(b, nir_f2u32(b, layer), nir_iadd_imm(b, cube_count, -1));
      }
      bool gather = tex->op == nir_texop_tg4;
      nir_def *lod = gather ? nir_imm_float(b, 0) :
         kk_layered_bc_cube_lod(b, tex, coord, face, size, dx, dy);
      if (tex->op == nir_texop_lod) {
         result = nir_vec2(b, nir_imm_float(b, 0), lod);
      } else {
         nir_def *filter = load_resource_deref_desc(b, 1, 16, sampler,
            offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits), ctx);
         nir_def *seamless = nir_inot(b, nir_test_mask(b, filter, KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE));
         /* The common accessor distinguishes native half metadata from
          * virtual fp32 records; pointer chunks are never numeric LODs. */
         nir_def *min = kk_sampler_query_metadata(b, tex,
            KK_SAMPLER_META_MIN_LOD, false);
         nir_def *max = kk_sampler_query_metadata(b, tex,
            KK_SAMPLER_META_MAX_LOD, false);
         lod = nir_fmin(b, nir_fmax(b, lod, min), max);
         nir_def *linear = gather ? nir_imm_bool(b, true) : nir_ine_imm(b,
            nir_iand(b, filter, nir_bcsel(b, nir_fle_imm(b, lod, 0),
               nir_imm_intN_t(b, 2, 16), nir_imm_intN_t(b, 1, 16))), 0);
         nir_def *mode = gather ? nir_imm_int(b, 0) : nir_f2u32(b,
            kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_REDUCTION_MODE, false));
         result = kk_layered_bc_cube_footprint(b, tex, table, cube, face,
            uv, size, seamless, linear, mode, coord, dx, dy);
      }
   }
   nir_push_else(b, NULL);
   nir_def *invalid = nir_imm_zero(b, tex->def.num_components, tex->def.bit_size);
   nir_pop_if(b, NULL);
   result = nir_if_phi(b, result, invalid);
   nir_push_else(b, NULL);
   nir_tex_instr *native = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &native->instr);
   b->cursor = nir_before_instr(&native->instr);
   if (implicit && tex->op != nir_texop_lod) {
      nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
      nir_def *scale = bias ? nir_fexp2(b, nir_f2f32(b, bias)) : nir_imm_float(b, 1);
      nir_steal_tex_src(native, nir_tex_src_bias);
      native->op = nir_texop_txd;
      nir_tex_instr_add_src(native, nir_tex_src_ddx, nir_fmul(b, dx, scale));
      nir_tex_instr_add_src(native, nir_tex_src_ddy, nir_fmul(b, dy, scale));
   }
   b->cursor = nir_after_instr(&native->instr);
   nir_pop_if(b, NULL);
   nir_def_replace(&tex->def, nir_if_phi(b, result, &native->def));
   return true;
}
