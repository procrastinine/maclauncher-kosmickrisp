/*
 * Copyright 2016 Broadcom
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared LOD and cube-coordinate utilities for sampler emulation.
 * Sample footprints are emitted by the common MSL sampler helpers.
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
          probe->src[i].src_type != nir_tex_src_texture_handle &&
          probe->src[i].src_type != nir_tex_src_sampler_handle &&
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
   /* Only the temporary probe needs lowering. Walking the growing shader
    * here for every sample would make LOD construction quadratic. */
   const nir_lower_tex_options options = {0};
   nir_lower_tex_instr_gradient(b, probe, &options);
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
                nir_def *linear, nir_def *mode, nir_def *unnormalized,
                bool weighted, nir_def *(*tap_cb)(nir_builder *,nir_tex_instr *,
                   nir_def *,nir_def *,void *), void *tap_data)
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
      nir_def *factor = NULL;
      if (weighted) {
         factor = nir_imm_float(b, 1);
         for (unsigned axis = 0; axis < dims; axis++) {
            nir_def *f = nir_channel(b, fraction, axis);
            if (!(corner & (1u << axis))) f = nir_fsub(b, nir_imm_float(b, 1), f);
            factor = nir_fmul(b, factor, f);
         }
         factor = nir_bcsel(b, linear, factor,
            nir_imm_float(b, corner == 0 ? 1 : 0));
      }
      nir_def *center = nir_vec(b, components, dims);
      center = nir_bcsel(b, unnormalized, center, nir_fdiv(b, center, size));
      if (cube)
         center = kk_minmax_cube_direction(b, center, face);
      if (tex->is_array)
         center = nir_vector_insert_imm(b, nir_pad_vector(b, center, center->num_components + 1),
            nir_channel(b, coord, coord->num_components - 1), center->num_components);
      if (corner == 0) {
         result = tap_cb ? tap_cb(b, tex, center, level, tap_data) :
                           kk_minmax_tap(b, tex, center, level);
         if (weighted && nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
            nir_def *color = tex->is_sparse ? nir_trim_vector(b, result, 4) : result;
            nir_def *scaled = nir_fmul(b, color, nir_f2fN(b, factor, color->bit_size));
            color = nir_bcsel(b, nir_ieq_imm(b, mode, 0), scaled, color);
            result = tex->is_sparse ? nir_vector_insert_imm(b,
               nir_pad_vector(b, color, 5), nir_channel(b, result, 4), 4) : color;
         }
      } else {
         nir_push_if(b, weight);
         nir_def *tap = tap_cb ? tap_cb(b, tex, center, level, tap_data) :
                              kk_minmax_tap(b, tex, center, level);
         nir_def *combined = kk_minmax_combine(b, result, tap, mode, tex->is_sparse, tex->dest_type);
         if (weighted && nir_alu_type_get_base_type(tex->dest_type) == nir_type_float) {
            nir_def *a = tex->is_sparse ? nir_trim_vector(b, result, 4) : result;
            nir_def *c = tex->is_sparse ? nir_trim_vector(b, tap, 4) : tap;
            nir_def *sum = nir_fadd(b, a, nir_fmul(b, c, nir_f2fN(b, factor, c->bit_size)));
            if (tex->is_sparse) sum = nir_vector_insert_imm(b, nir_pad_vector(b, sum, 5),
               nir_channel(b, combined, 4), 4);
            combined = nir_bcsel(b, nir_ieq_imm(b, mode, 0), sum, combined);
         }
         nir_pop_if(b, NULL);
         result = nir_if_phi(b, combined, result);
      }
   }
   return result;
}

#endif
