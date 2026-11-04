/*
 * Copyright 2016 Broadcom
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * Footprint construction follows Mesa nir_lower_tex.c. The centered-tap
 * reduction technique is also used by MetalSharp's SPIRV-Cross adaptation.
 * Metal gather has no explicit mip-level operand; explicit-level taps keep
 * mip selection, addressing and border handling in the shared texture path.
 */
#ifndef KK_SAMPLER_MINMAX_H
#define KK_SAMPLER_MINMAX_H
#include "kk_sampler.h"
#include "nir_builtin_builder.h"

static nir_def *
kk_minmax_metadata(nir_builder *b, nir_tex_instr *tex, nir_deref_instr *sampler, unsigned offset,
                  const struct lower_descriptors_ctx *ctx)
{
   enum kk_sampler_metadata_value field;
   switch (offset % sizeof(struct kk_sampled_image_descriptor)) {
   case offsetof(struct kk_sampled_image_descriptor, sampler_lod_bias_fp16):
      field = KK_SAMPLER_META_BIAS; break;
   case offsetof(struct kk_sampled_image_descriptor, sampler_lod_min_fp16):
      field = KK_SAMPLER_META_MIN_LOD; break;
   case offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16):
      field = KK_SAMPLER_META_MAX_LOD; break;
   case offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode):
      return nir_f2u16(b, kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_MIPMAP, false));
   default:
      return load_resource_deref_desc(b, 1, 16, sampler, offset, ctx);
   }
   return kk_sampler_query_metadata(b, tex, field, true);
}

static nir_def *
kk_minmax_combine(nir_builder *b, nir_def *a, nir_def *c, nir_def *mode, bool sparse, nir_alu_type type)
{
   nir_def *status = sparse ? nir_ior(b, nir_channel(b, a, 4), nir_channel(b, c, 4)) : NULL;
   if (sparse) {
      a = nir_trim_vector(b, a, 4);
      c = nir_trim_vector(b, c, 4);
   }
   nir_def *minimum, *maximum;
   switch (nir_alu_type_get_base_type(type)) {
   case nir_type_int: minimum = nir_imin(b, a, c); maximum = nir_imax(b, a, c); break;
   case nir_type_uint: minimum = nir_umin(b, a, c); maximum = nir_umax(b, a, c); break;
   default: minimum = nir_fmin(b, a, c); maximum = nir_fmax(b, a, c); break;
   }
   nir_def *value = nir_bcsel(b, nir_ieq_imm(b, mode, VK_SAMPLER_REDUCTION_MODE_MIN),
                             minimum, maximum);
   return sparse ? nir_vector_insert_imm(b, nir_pad_vector(b, value, 5), status, 4) : value;
}

static bool
kk_minmax_probe_only(const nir_tex_instr *tex, const void *probe)
{
   return tex == probe;
}

/* Use Mesa's dimension-aware gradient calculation, including the cube
 * quotient rule. The probe is never sampled and no original sample is
 * converted to scalar LOD, so its native anisotropic path is preserved. */
static nir_def *
kk_minmax_lod(nir_builder *b, nir_tex_instr *tex, nir_def *dx, nir_def *dy)
{
   nir_def *lod = nir_get_tex_src(tex, nir_tex_src_lod);
   if (lod)
      return nir_f2f32(b, lod);

   nir_tex_instr *probe = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &probe->instr);
   b->cursor = nir_before_instr(&probe->instr);
   for (int i = probe->num_srcs - 1; i >= 0; --i)
      if (probe->src[i].src_type != nir_tex_src_texture_deref &&
          probe->src[i].src_type != nir_tex_src_sampler_deref &&
          probe->src[i].src_type != nir_tex_src_coord)
         nir_tex_instr_remove_src(probe, i);
   /* Query coordinates exclude the array index. The never-sampled gradient
    * probe is a txd, whose IR coordinates include it; append a dummy layer
    * without dropping any spatial derivative or changing the resource type. */
   if (tex->op == nir_texop_lod && tex->is_array) {
      int c = nir_tex_instr_src_index(probe, nir_tex_src_coord);
      nir_def *coord = probe->src[c].src.ssa;
      nir_def *array_coord = nir_vector_insert_imm(b,
         nir_pad_vector(b, coord, coord->num_components + 1),
         nir_imm_float(b, 0), coord->num_components);
      nir_src_rewrite(&probe->src[c].src, array_coord);
      probe->coord_components = array_coord->num_components;
   }
   probe->op = nir_texop_txd;
   nir_tex_instr_add_src(probe, nir_tex_src_ddx, dx);
   nir_tex_instr_add_src(probe, nir_tex_src_ddy, dy);
   const nir_lower_tex_options options = {
      .lower_txd_cb = kk_minmax_probe_only,
      .lower_txd_data = probe,
   };
   nir_lower_tex(b->shader, &options);
   lod = nir_get_tex_src(probe, nir_tex_src_lod);
   b->cursor = nir_before_instr(&probe->instr);
   nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
   if (bias)
      lod = nir_fadd(b, lod, nir_f2f32(b, bias));
   b->cursor = nir_instr_remove(&probe->instr);
   return lod;
}

/* Convert cube direction to face-local UV; reconstructing directions from
 * centers outside a face lets Metal apply seamless adjacent-face addressing. */
static nir_def *
kk_minmax_cube_uv(nir_builder *b, nir_def *coord, nir_def **face)
{
   nir_def *x = nir_channel(b, coord, 0), *y = nir_channel(b, coord, 1);
   nir_def *z = nir_channel(b, coord, 2);
   nir_def *ax = nir_fabs(b, x), *ay = nir_fabs(b, y), *az = nir_fabs(b, z);
   nir_def *is_x = nir_iand(b, nir_fge(b, ax, ay), nir_fge(b, ax, az));
   nir_def *is_y = nir_fge(b, ay, az);
   nir_def *neg_x = nir_flt_imm(b, x, 0), *neg_y = nir_flt_imm(b, y, 0);
   nir_def *neg_z = nir_flt_imm(b, z, 0);
   nir_def *u = nir_bcsel(b, is_x, nir_bcsel(b, neg_x, z, nir_fneg(b, z)),
      nir_bcsel(b, is_y, x, nir_bcsel(b, neg_z, nir_fneg(b, x), x)));
   nir_def *v = nir_bcsel(b, is_x, nir_fneg(b, y),
      nir_bcsel(b, is_y, nir_bcsel(b, neg_y, nir_fneg(b, z), z), nir_fneg(b, y)));
   nir_def *ma = nir_fmax(b, ax, nir_fmax(b, ay, az));
   *face = nir_bcsel(b, is_x, nir_bcsel(b, neg_x, nir_imm_int(b, 1), nir_imm_int(b, 0)),
      nir_bcsel(b, is_y, nir_bcsel(b, neg_y, nir_imm_int(b, 3), nir_imm_int(b, 2)),
         nir_bcsel(b, neg_z, nir_imm_int(b, 5), nir_imm_int(b, 4))));
   return nir_fadd_imm(b, nir_fmul(b, nir_vec2(b, u, v), nir_frcp(b, ma)), 1.0);
}

static nir_def *
kk_minmax_cube_direction(nir_builder *b, nir_def *uv, nir_def *face)
{
   nir_def *u = nir_fadd_imm(b, nir_fmul_imm(b, nir_channel(b, uv, 0), 2), -1);
   nir_def *v = nir_fadd_imm(b, nir_fmul_imm(b, nir_channel(b, uv, 1), 2), -1);
   nir_def *one = nir_imm_float(b, 1), *minus = nir_imm_float(b, -1);
   nir_def *dirs[] = {
      nir_vec3(b, one, nir_fneg(b, v), nir_fneg(b, u)),
      nir_vec3(b, minus, nir_fneg(b, v), u),
      nir_vec3(b, u, one, v), nir_vec3(b, u, minus, nir_fneg(b, v)),
      nir_vec3(b, u, nir_fneg(b, v), one),
      nir_vec3(b, nir_fneg(b, u), nir_fneg(b, v), minus),
   };
   nir_def *r = dirs[5];
   for (int i = 4; i >= 0; --i)
      r = nir_bcsel(b, nir_ieq_imm(b, face, i), dirs[i], r);
   return r;
}

static nir_def *
kk_minmax_tap(nir_builder *b, nir_tex_instr *tex, nir_def *coord, nir_def *level)
{
   nir_tex_instr *tap = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &tap->instr);
   b->cursor = nir_before_instr(&tap->instr);
   for (int i = tap->num_srcs - 1; i >= 0; --i)
      if (tap->src[i].src_type != nir_tex_src_texture_deref &&
          tap->src[i].src_type != nir_tex_src_sampler_deref &&
          tap->src[i].src_type != nir_tex_src_plane)
         nir_tex_instr_remove_src(tap, i);
   tap->op = nir_texop_txl;
   nir_tex_instr_add_src(tap, nir_tex_src_coord, coord);
   nir_tex_instr_add_src(tap, nir_tex_src_lod, nir_u2f32(b, level));
   b->cursor = nir_after_instr(&tap->instr);
   return &tap->def;
}

static nir_def *
kk_minmax_level(nir_builder *b, nir_tex_instr *tex, nir_def *level,
                nir_def *linear, nir_def *mode, nir_def *unnormalized)
{
   bool cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
   unsigned dims = tex->sampler_dim == GLSL_SAMPLER_DIM_3D ? 3 : 2;
   nir_def *size = nir_build_texture_query(b, tex, nir_texop_txs,
      dims + tex->is_array, nir_type_uint32, false, false);
   nir_tex_instr *query = nir_instr_as_tex(nir_def_instr(size));
   nir_tex_instr_add_src(query, nir_tex_src_lod, level);
   size = nir_u2f32(b, nir_trim_vector(b, size, dims));
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *face = NULL;
   nir_def *uv = cube ? nir_fmul_imm(b, kk_minmax_cube_uv(b, coord, &face), 0.5)
                       : nir_trim_vector(b, coord, dims);
   nir_def *scaled = nir_bcsel(b, unnormalized, uv, nir_fmul(b, uv, size));
   nir_def *p = nir_fsub(b, scaled, nir_bcsel(b, linear,
      nir_imm_float(b, 0.5), nir_imm_float(b, 0)));
   nir_def *base = nir_ffloor(b, p), *fraction = nir_fsub(b, p, base);
   nir_def *offset = nir_get_tex_src(tex, nir_tex_src_offset);
   if (offset)
      base = nir_fadd(b, base, nir_i2f32(b, offset));
   nir_def *result = NULL;
   for (unsigned corner = 0; corner < (1u << dims); ++corner) {
      nir_def *components[3], *weight = linear;
      for (unsigned axis = 0; axis < dims; ++axis) {
         bool hi = corner & (1u << axis);
         components[axis] = nir_fadd_imm(b, nir_channel(b, base, axis), hi ? 1.5 : 0.5);
         if (hi)
            weight = nir_iand(b, weight, nir_fgt_imm(b, nir_channel(b, fraction, axis), 0));
      }
      nir_def *center = nir_vec(b, components, dims);
      center = nir_bcsel(b, unnormalized, center, nir_fdiv(b, center, size));
      if (cube)
         center = kk_minmax_cube_direction(b, center, face);
      if (tex->is_array)
         center = nir_vector_insert_imm(b, nir_pad_vector(b, center, center->num_components + 1),
            nir_channel(b, coord, coord->num_components - 1), center->num_components);
      if (corner == 0) {
         result = kk_minmax_tap(b, tex, center, level);
      } else {
         nir_push_if(b, weight);
         nir_def *tap = kk_minmax_tap(b, tex, center, level);
         nir_def *combined = kk_minmax_combine(b, result, tap, mode, tex->is_sparse, tex->dest_type);
         nir_pop_if(b, NULL);
         result = nir_if_phi(b, combined, result);
      }
   }
   return result;
}

static bool
kk_lower_minmax_sample(nir_builder *b, nir_tex_instr *tex, void *data)
{
   const struct lower_descriptors_ctx *ctx = data;
   if (tex->is_shadow ||
       (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
        tex->op != nir_texop_txl && tex->op != nir_texop_txd && tex->op != nir_texop_lod))
      return false;
   nir_def *sampler_src = nir_get_tex_src(tex, nir_tex_src_sampler_deref);
   if (!sampler_src)
      return false;
   nir_deref_instr *sampler = nir_src_as_deref(nir_src_for_ssa(sampler_src));
   nir_variable *var = nir_deref_instr_get_variable(sampler);
   const struct kk_descriptor_set_binding_layout *binding = var ?
      get_binding_layout(var->data.descriptor_set, var->data.binding, ctx) : NULL;
   if (binding && binding->immutable_samplers) {
      bool any = false;
      for (unsigned i = 0; i < binding->array_size; ++i)
         any |= binding->immutable_samplers[i]->reduction_mode != 0;
      if (!any)
         return false;
   }
   if (tex->op == nir_texop_lod) {
      /* The native sampler is configured for individual taps. Its unclamped
       * query remains valid; reconstruct the requested sampler's clamped
       * query using the original metadata rather than the tap sampler. */
      b->cursor = nir_after_instr(&tex->instr);
      nir_def *mode = kk_minmax_metadata(b, tex, sampler,
         offsetof(struct kk_sampled_image_descriptor, sampler_reduction_mode), ctx);
      nir_def *min = nir_f2f32(b, kk_minmax_metadata(b, tex, sampler,
         offsetof(struct kk_sampled_image_descriptor, sampler_lod_min_fp16), ctx));
      nir_def *max = nir_f2f32(b, kk_minmax_metadata(b, tex, sampler,
         offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16), ctx));
      nir_def *mipmap = kk_minmax_metadata(b, tex, sampler,
         offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode), ctx);
      nir_def *lod = nir_channel(b, &tex->def, 1);
      nir_def *clamped = nir_fmin(b, nir_fmax(b, lod, min), max);
      nir_def *levels = nir_build_texture_query(b, tex, nir_texop_query_levels,
         1, nir_type_uint32, false, false);
      clamped = nir_fclamp(b, clamped, nir_imm_float(b, 0),
         nir_u2f32(b, nir_iadd_imm(b, levels, -1)));
      nir_def *nearest = nir_fmax(b, nir_fadd_imm(b,
         nir_fceil(b, nir_fadd_imm(b, clamped, 0.5)), -1), nir_imm_float(b, 0));
      clamped = nir_bcsel(b, nir_ieq_imm(b, mipmap, VK_SAMPLER_MIPMAP_MODE_NEAREST),
                          nearest, clamped);
      nir_def *value = nir_bcsel(b, nir_ine_imm(b, mode, 0),
                                 nir_vec2(b, clamped, lod), &tex->def);
      nir_def_rewrite_uses_after(&tex->def, value);
      return true;
   }
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *mode = kk_minmax_metadata(b, tex, sampler,
      offsetof(struct kk_sampled_image_descriptor, sampler_reduction_mode), ctx);
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *dx = nir_get_tex_src(tex, nir_tex_src_ddx);
   nir_def *dy = nir_get_tex_src(tex, nir_tex_src_ddy);
   bool implicit = tex->op == nir_texop_tex || tex->op == nir_texop_txb;
   if (implicit) {
      nir_def *spatial = tex->is_array ? nir_trim_vector(b, coord, coord->num_components - 1) : coord;
      dx = nir_ddx(b, spatial);
      dy = nir_ddy(b, spatial);
   }
   nir_push_if(b, nir_ine_imm(b, mode, 0));
   nir_def *lod = kk_minmax_lod(b, tex, dx, dy);
   nir_def *min = nir_f2f32(b, kk_minmax_metadata(b, tex, sampler,
      offsetof(struct kk_sampled_image_descriptor, sampler_lod_min_fp16), ctx));
   nir_def *max = nir_f2f32(b, kk_minmax_metadata(b, tex, sampler,
      offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16), ctx));
   lod = nir_fmin(b, nir_fmax(b, lod, min), max);
   nir_def *image_min = nir_get_tex_src(tex, nir_tex_src_min_lod);
   if (image_min)
      lod = nir_fmax(b, lod, nir_f2f32(b, image_min));
   nir_def *filter = kk_minmax_metadata(b, tex, sampler,
      offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits), ctx);
   nir_def *linear = nir_ine_imm(b, nir_iand(b, filter,
      nir_bcsel(b, nir_fle_imm(b, lod, 0), nir_imm_intN_t(b, 2, 16),
                nir_imm_intN_t(b, 1, 16))), 0);
   nir_def *levels = nir_build_texture_query(b, tex, nir_texop_query_levels,
      1, nir_type_uint32, false, false);
   lod = nir_fclamp(b, lod, nir_imm_float(b, 0), nir_u2f32(b, nir_iadd_imm(b, levels, -1)));
   nir_def *mipmap = kk_minmax_metadata(b, tex, sampler,
      offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode), ctx);
   nir_def *mip_linear = nir_ieq_imm(b, mipmap, VK_SAMPLER_MIPMAP_MODE_LINEAR);
   nir_def *nearest = nir_fmax(b, nir_fadd_imm(b, nir_fceil(b, nir_fadd_imm(b, lod, 0.5)), -1),
                               nir_imm_float(b, 0));
   nir_def *low = nir_f2u32(b, nir_bcsel(b, mip_linear, nir_ffloor(b, lod), nearest));
   nir_def *address = kk_minmax_metadata(b, tex, sampler,
      offsetof(struct kk_sampled_image_descriptor, sampler_address_modes), ctx);
   nir_def *unnormalized = nir_test_mask(b, address, 1u << 9);
   nir_def *result = kk_minmax_level(b, tex, low, linear, mode, unnormalized);
   nir_def *next = nir_iadd_imm(b, low, 1);
   nir_push_if(b, nir_iand(b, mip_linear, nir_iand(b,
      nir_flt(b, nir_u2f32(b, low), lod), nir_ult(b, next, levels))));
   nir_def *upper = kk_minmax_level(b, tex, next, linear, mode, unnormalized);
   nir_def *combined = kk_minmax_combine(b, result, upper, mode, tex->is_sparse, tex->dest_type);
   nir_pop_if(b, NULL);
   result = nir_if_phi(b, combined, result);
   nir_push_else(b, NULL);
   nir_tex_instr *native = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   nir_builder_instr_insert(b, &native->instr);
   b->cursor = nir_before_instr(&native->instr);
   if (implicit) {
      nir_def *bias_value = nir_get_tex_src(tex, nir_tex_src_bias);
      if (bias_value) {
         nir_def *scale = nir_fexp2(b, nir_f2f32(b, bias_value));
         dx = nir_fmul(b, dx, scale);
         dy = nir_fmul(b, dy, scale);
      }
      int bias = nir_tex_instr_src_index(native, nir_tex_src_bias);
      if (bias >= 0)
         nir_tex_instr_remove_src(native, bias);
      native->op = nir_texop_txd;
      nir_tex_instr_add_src(native, nir_tex_src_ddx, dx);
      nir_tex_instr_add_src(native, nir_tex_src_ddy, dy);
   }
   b->cursor = nir_after_instr(&native->instr);
   nir_pop_if(b, NULL);
   nir_def *phi = nir_if_phi(b, result, &native->def);
   nir_def_replace(&tex->def, phi);
   return true;
}

bool
kk_nir_lower_sampler_minmax(nir_shader *nir, uint32_t set_layout_count,
                            struct vk_descriptor_set_layout *const *set_layouts)
{
   struct lower_descriptors_ctx ctx = {};
   assert(set_layout_count <= KK_MAX_SETS);
   for (unsigned i = 0; i < set_layout_count; ++i)
      if (set_layouts[i])
         ctx.set_layouts[i] = vk_to_kk_descriptor_set_layout(set_layouts[i]);
   return nir_shader_tex_pass(nir, kk_lower_minmax_sample, nir_metadata_none, &ctx);
}
#endif
