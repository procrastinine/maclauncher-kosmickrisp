/*
 * Copyright 2021 Google
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "kk_ray_tracing.h"
#include "kosmickrisp/as/kosmickrisp-as-types.h"
#include "compiler/spirv/spirv.h"
#include "vulkan/vulkan_core.h"
#include <float.h>
#include <math.h>

/* Loads stay inside their controlling null/range branches throughout NIR
 * optimization. No RT global load is declared speculatable. */
static nir_def *
kk_rt_load(nir_builder *b, nir_def *address, uint32_t offset, unsigned components, unsigned bits)
{
   return nir_load_global(b, components, bits, nir_iadd_imm(b, address, offset),
                           .align_mul = bits / 8, .access = ACCESS_VOLATILE);
}

static nir_def *
kk_rt_load_node_data(nir_builder *b, nir_def *unused, nir_def **data, uint32_t offset)
{
   assert(data && offset < 9 * sizeof(float));
   return data[offset / sizeof(float)];
}

/* The exact common watertight triangle arithmetic and matrix multiplication
 * are projected from the selected Mesa source with their original notices. */
#include "kk_rt_math.h"

void
kk_rt_load_wto_matrix(nir_builder *b, nir_def *payload, nir_def **unused, nir_def **out)
{
   for (unsigned row = 0; row < 3; ++row)
      out[row] = kk_rt_load(b, payload, (12 + row * 4) * sizeof(float), 4, 32);
}

nir_def *
kk_rt_load_vertex_position(nir_builder *b, nir_def *payload, uint32_t index)
{
   return kk_rt_load(b, payload, index * 3 * sizeof(float), 3, 32);
}

static nir_def *
kk_rt_box_intersects(nir_builder *b, nir_def *address, const struct kk_rt_ray_traversal_args *args)
{
   nir_def *origin = nir_load_deref(b, args->vars.origin);
   nir_def *dir = nir_load_deref(b, args->vars.dir);
   nir_def *inv = nir_load_deref(b, args->vars.inv_dir);
   nir_def *min = kk_rt_load(b, address, offsetof(struct kk_as_node, min), 3, 32);
   nir_def *max = kk_rt_load(b, address, offsetof(struct kk_as_node, max), 3, 32);
   nir_def *a = nir_fmul(b, nir_fsub(b, min, origin), inv);
   nir_def *z = nir_fmul(b, nir_fsub(b, max, origin), inv);
   /* A zero direction component leaves its slab unbounded when the origin
    * lies within it, boundary included, and empty otherwise. This keeps rays
    * along a shared triangle edge inside both neighbours' boxes. */
   nir_def *axis = nir_feq_imm(b, dir, 0);
   nir_def *inside = nir_iand(b, nir_fge(b, origin, min), nir_fge(b, max, origin));
   nir_def *inf = nir_imm_vec3(b, FLT_MAX, FLT_MAX, FLT_MAX);
   nir_def *ninf = nir_imm_vec3(b, -FLT_MAX, -FLT_MAX, -FLT_MAX);
   nir_def *near = nir_bcsel(b, axis, nir_bcsel(b, inside, ninf, inf), nir_fmin(b, a, z));
   nir_def *far = nir_bcsel(b, axis, nir_bcsel(b, inside, inf, ninf), nir_fmax(b, a, z));
   nir_def *near_t = nir_fmax(b, nir_channel(b, near, 0),
                               nir_fmax(b, nir_channel(b, near, 1), nir_channel(b, near, 2)));
   nir_def *far_t = nir_fmin(b, nir_channel(b, far, 0),
                              nir_fmin(b, nir_channel(b, far, 1), nir_channel(b, far, 2)));
   nir_def *valid = nir_iand(b,
      nir_fge(b, nir_channel(b, max, 0), nir_channel(b, min, 0)),
      nir_iand(b, nir_fge(b, nir_channel(b, max, 1), nir_channel(b, min, 1)),
                  nir_fge(b, nir_channel(b, max, 2), nir_channel(b, min, 2))));
   return nir_iand(b, valid,
      nir_iand(b, nir_fge(b, far_t, nir_fmax(b, args->tmin, near_t)),
                  nir_flt(b, near_t, nir_load_deref(b, args->vars.tmax))));
}

static nir_def *
kk_rt_opaque(nir_builder *b, nir_def *geometry, const struct kk_rt_ray_traversal_args *args,
               const struct kk_rt_ray_flags *flags)
{
   nir_def *instance = nir_load_deref(b, args->vars.sbt_offset_and_flags);
   nir_def *opaque = nir_test_mask(b, geometry, 1u << 31);
   opaque = nir_ior(b, opaque, nir_test_mask(b, instance, 4u << 24));
   opaque = nir_iand(b, opaque, nir_inot(b, nir_test_mask(b, instance, 8u << 24)));
   opaque = nir_bcsel(b, flags->force_opaque, nir_imm_true(b), opaque);
   return nir_bcsel(b, flags->force_not_opaque, nir_imm_false(b), opaque);
}

static struct kk_rt_leaf_intersection
kk_rt_leaf(nir_builder *b, nir_def *node, const struct kk_rt_ray_traversal_args *args,
              const struct kk_rt_ray_flags *flags)
{
   struct kk_rt_leaf_intersection hit = {.node_addr = node};
   hit.primitive_id = kk_rt_load(b, node, offsetof(struct kk_as_node, primitive_id), 1, 32);
   nir_def *geometry = kk_rt_load(b, node, offsetof(struct kk_as_node, geometry_id), 1, 32);
   nir_def *opaque = nir_ishl_imm(b, nir_iand_imm(b,
      kk_rt_load(b, node, offsetof(struct kk_as_node, flags), 1, 32), 1), 31);
   hit.geometry_id_and_flags = nir_ior(b, geometry, opaque);
   hit.opaque = kk_rt_opaque(b, hit.geometry_id_and_flags, args, flags);
   return hit;
}

/* Exclusive prefix XOR within each 16-bit lane, and bit interleaving of a
 * 16-bit value: the bird-curve helpers of opacity micromap indexing. */
static nir_def *
kk_rt_prefix_xor(nir_builder *b, nir_def *x)
{
   static const uint32_t masks[] = {0x7fff7fff, 0x3fff3fff, 0x0fff0fff, 0x00ff00ff};
   for (unsigned i = 0; i < ARRAY_SIZE(masks); ++i)
      x = nir_ixor(b, x, nir_iand_imm(b, nir_ushr_imm(b, x, 1u << i), masks[i]));
   return x;
}

static nir_def *
kk_rt_interleave(nir_builder *b, nir_def *x)
{
   static const uint32_t masks[] = {0x00ff00ff, 0x0f0f0f0f, 0x33333333, 0x55555555};
   for (unsigned i = 0; i < ARRAY_SIZE(masks); ++i)
      x = nir_iand_imm(b, nir_ior(b, x, nir_ishl_imm(b, x, 8u >> i)), masks[i]);
   return x;
}

/* Index of the micro-triangle that contains barycentrics (u, v) at a
 * subdivision level: discrete barycentrics, then the bird-curve position. */
static nir_def *
kk_rt_micro_triangle(nir_builder *b, nir_def *bary, nir_def *level)
{
   nir_def *n = nir_u2f32(b, nir_ishl(b, nir_imm_int(b, 1), level));
   nir_def *mask = nir_iadd_imm(b, nir_ishl(b, nir_imm_int(b, 1), level), -1);
   nir_def *u = nir_channel(b, bary, 0), *v = nir_channel(b, bary, 1);
   nir_def *w = nir_fsub(b, nir_fsub_imm(b, 1.0f, u), v);
   nir_def *d[3];
   nir_def *c[3] = {u, v, w};
   for (unsigned i = 0; i < 3; ++i)
      d[i] = nir_umin(b, nir_f2u32(b, nir_fmax(b, nir_ffloor(b, nir_fmul(b, c[i], n)),
                                              nir_imm_float(b, 0))), mask);
   nir_def *b0 = nir_iand(b, nir_inot(b, nir_ixor(b, d[0], d[2])), mask);
   nir_def *t = nir_iand(b, nir_ixor(b, d[0], d[1]), b0);
   nir_def *f = nir_ixor(b, kk_rt_prefix_xor(b, t), d[0]);
   nir_def *b1 = nir_iand(b, nir_ior(b, nir_iand(b, f, nir_inot(b, b0)), t), mask);
   return nir_ior(b, kk_rt_interleave(b, b0), nir_ishl_imm(b, kk_rt_interleave(b, b1), 1));
}

/* Opacity micromap state of a triangle hit: 0 transparent, 1 opaque,
 * 2 unknown transparent, 3 unknown opaque. Special indices -1..-4 give the
 * state directly (state = ~index). */
static nir_def *
kk_rt_micromap_state(nir_builder *b, nir_def *payload, nir_def *bary)
{
   nir_def *index = kk_rt_load(b, payload, 9 * sizeof(float), 1, 32);
   nir_push_if(b, nir_ilt_imm(b, index, 0));
   nir_def *special = nir_iand_imm(b, nir_inot(b, index), 3);
   nir_push_else(b, NULL);
   nir_def *micromap = kk_rt_load(b, payload, 10 * sizeof(float), 1, 64);
   nir_def *records = nir_iadd(b, micromap,
      kk_rt_load(b, micromap, offsetof(struct kk_as_header, payload_offset), 1, 64));
   nir_def *record = nir_iadd(b, records, nir_imul_imm(b, nir_u2u64(b, index),
      sizeof(struct kk_as_micromap_triangle)));
   nir_def *data_offset = kk_rt_load(b, record, 0, 1, 32);
   nir_def *word = kk_rt_load(b, record, 4, 1, 32);
   nir_def *level = nir_iand_imm(b, word, 0xffff);
   nir_def *format = nir_ushr_imm(b, word, 16);
   nir_def *micro = kk_rt_micro_triangle(b, bary, level);
   nir_def *four = nir_ieq_imm(b, format, 2);
   nir_def *bit = nir_bcsel(b, four, nir_ishl_imm(b, micro, 1), micro);
   nir_def *data = nir_iadd(b, micromap,
      kk_rt_load(b, micromap, offsetof(struct kk_as_header, geometry_offset), 1, 64));
   /* The packed states start 16-byte aligned; read the containing word. */
   nir_def *byte_index = nir_iadd(b, data_offset, nir_ushr_imm(b, bit, 3));
   nir_def *address = nir_iadd(b, data, nir_u2u64(b, nir_iand_imm(b, byte_index, ~3u)));
   nir_def *shift = nir_iadd(b, nir_ishl_imm(b, nir_iand_imm(b, byte_index, 3), 3),
                             nir_iand_imm(b, bit, 7));
   nir_def *bits = nir_ushr(b, kk_rt_load(b, address, 0, 1, 32), shift);
   nir_def *stored = nir_bcsel(b, four, nir_iand_imm(b, bits, 3), nir_iand_imm(b, bits, 1));
   /* A record without states (format 0) reads as unknown opaque. */
   stored = nir_bcsel(b, nir_ieq_imm(b, format, 0), nir_imm_int(b, 3), stored);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, special, stored);
}

static void
kk_rt_triangle(nir_builder *b, nir_def *node, const struct kk_rt_ray_traversal_args *args,
                  const struct kk_rt_ray_flags *flags)
{
   if (!args->triangle_cb)
      return;
   nir_def *payload = nir_iadd(b, nir_load_deref(b, args->vars.bvh_base),
      kk_rt_load(b, node, offsetof(struct kk_as_node, child0_or_payload), 1, 64));
   nir_def *vertices[9];
   for (unsigned i = 0; i < ARRAY_SIZE(vertices); ++i)
      vertices[i] = kk_rt_load(b, payload, i * sizeof(float), 1, 32);
   nir_def *result = kk_rt_build_intersect_ray_tri(b, vertices,
      nir_load_deref(b, args->vars.tmax), nir_load_deref(b, args->vars.origin),
      nir_load_deref(b, args->vars.dir), nir_load_deref(b, args->vars.inv_dir));
   struct kk_rt_triangle_intersection hit = {.base = kk_rt_leaf(b, node, args, flags)};
   hit.base.node_addr = payload;
   hit.t = nir_channel(b, result, 0);
   hit.barycentrics = nir_channels(b, result, 0xc);
   nir_def *transparent = NULL;
   if (b->shader->info.ray_query_opacity_micromap) {
      /* The micromap state replaces the geometry's opaque bit; instance and
       * ray force-opaque flags still apply on top, and transparent
       * micro-triangles are skipped. */
      nir_def *instance = nir_load_deref(b, args->vars.sbt_offset_and_flags);
      nir_def *active = nir_iand(b,
         nir_test_mask(b, kk_rt_load(b, node, offsetof(struct kk_as_node, flags), 1, 32),
                       KK_AS_NODE_FLAG_OPACITY_MICROMAP),
         nir_inot(b, nir_test_mask(b, instance,
                  VK_GEOMETRY_INSTANCE_DISABLE_OPACITY_MICROMAPS_BIT_KHR << 24)));
      nir_push_if(b, active);
      nir_def *state = kk_rt_micromap_state(b, payload, hit.barycentrics);
      nir_def *two_state = nir_ior(b, nir_test_mask(b, args->flags, SpvRayFlagsForceOpacityMicromap2StateKHRMask),
         nir_test_mask(b, instance, VK_GEOMETRY_INSTANCE_FORCE_OPACITY_MICROMAP_2_STATE_BIT_KHR << 24));
      state = nir_bcsel(b, two_state, nir_iand_imm(b, state, 1), state);
      nir_def *geometry = nir_iand_imm(b, hit.base.geometry_id_and_flags, ~(1u << 31));
      geometry = nir_ior(b, geometry, nir_bcsel(b, nir_ieq_imm(b, state, 1),
                                                nir_imm_int(b, 1u << 31), nir_imm_int(b, 0)));
      nir_def *opaque = kk_rt_opaque(b, geometry, args, flags);
      nir_def *skip = nir_ieq_imm(b, state, 0);
      nir_pop_if(b, NULL);
      hit.base.geometry_id_and_flags = nir_if_phi(b, geometry, hit.base.geometry_id_and_flags);
      hit.base.opaque = nir_if_phi(b, opaque, hit.base.opaque);
      transparent = nir_if_phi(b, skip, nir_imm_false(b));
   }
   hit.frontface = nir_ixor(b, nir_fgt_imm(b, nir_channel(b, result, 1), 0),
      nir_test_mask(b, nir_load_deref(b, args->vars.sbt_offset_and_flags), 2u << 24));
   nir_def *not_culled = nir_ior(b,
      nir_bcsel(b, hit.frontface, flags->no_cull_front, flags->no_cull_back),
      nir_test_mask(b, nir_load_deref(b, args->vars.sbt_offset_and_flags), 1u << 24));
   not_culled = nir_iand(b, not_culled, flags->no_skip_triangles);
   not_culled = nir_iand(b, not_culled,
      nir_bcsel(b, hit.base.opaque, flags->no_cull_opaque, flags->no_cull_no_opaque));
   if (transparent)
      not_culled = nir_iand(b, not_culled, nir_inot(b, transparent));
   nir_push_if(b, nir_iand(b, not_culled,
      nir_iand(b, nir_flt(b, args->tmin, hit.t),
                  nir_flt(b, hit.t, nir_load_deref(b, args->vars.tmax)))));
   args->triangle_cb(b, &hit, args, flags);
   nir_pop_if(b, NULL);
}

static void
kk_rt_aabb(nir_builder *b, nir_def *node, const struct kk_rt_ray_traversal_args *args,
              const struct kk_rt_ray_flags *flags)
{
   if (!args->aabb_cb)
      return;
   struct kk_rt_leaf_intersection hit = kk_rt_leaf(b, node, args, flags);
   nir_push_if(b, nir_iand(b, flags->no_skip_aabbs,
      nir_bcsel(b, hit.opaque, flags->no_cull_opaque, flags->no_cull_no_opaque)));
   args->aabb_cb(b, &hit, args, flags);
   nir_pop_if(b, NULL);
}

void
kk_rt_begin_traversal(nir_builder *b, const struct kk_rt_ray_traversal_args *args)
{
   nir_store_deref(b, args->vars.current_node, nir_imm_int64(b, 0), 1);
   nir_store_deref(b, args->vars.tlas_resume, nir_imm_int64(b, 0), 1);
   nir_store_deref(b, args->vars.in_blas, nir_imm_false(b), 1);
   nir_store_deref(b, args->vars.instance_addr, nir_imm_int64(b, 0), 1);
   nir_store_deref(b, args->vars.instance_payload_addr, nir_imm_int64(b, 0), 1);
   nir_store_deref(b, args->vars.sbt_offset_and_flags, nir_imm_int(b, 0), 1);
   nir_push_if(b, nir_ine_imm(b, args->root_bvh_base, 0));
   nir_store_deref(b, args->vars.current_node,
      kk_rt_load(b, args->root_bvh_base, offsetof(struct kk_as_header, root_offset), 1, 64), 1);
   nir_pop_if(b, NULL);
}

nir_def *
kk_rt_build_ray_traversal(nir_builder *b, const struct kk_rt_ray_traversal_args *args)
{
   const unsigned saved_math = b->fp_math_ctrl;
   b->fp_math_ctrl = nir_fp_no_fast_math;
   nir_variable *incomplete = nir_local_variable_create(b->impl, glsl_bool_type(), "kk_rt_incomplete");
   nir_store_var(b, incomplete, nir_imm_true(b), 1);
   nir_def *ones = nir_imm_vec3(b, 1, 1, 1);
   struct kk_rt_ray_flags flags = {
      .force_opaque = nir_test_mask(b, args->flags, SpvRayFlagsOpaqueKHRMask),
      .force_not_opaque = nir_test_mask(b, args->flags, SpvRayFlagsNoOpaqueKHRMask),
      .terminate_on_first_hit = nir_test_mask(b, args->flags, SpvRayFlagsTerminateOnFirstHitKHRMask),
      .no_cull_front = nir_inot(b, nir_test_mask(b, args->flags, SpvRayFlagsCullFrontFacingTrianglesKHRMask)),
      .no_cull_back = nir_inot(b, nir_test_mask(b, args->flags, SpvRayFlagsCullBackFacingTrianglesKHRMask)),
      .no_cull_opaque = nir_inot(b, nir_test_mask(b, args->flags, SpvRayFlagsCullOpaqueKHRMask)),
      .no_cull_no_opaque = nir_inot(b, nir_test_mask(b, args->flags, SpvRayFlagsCullNoOpaqueKHRMask)),
      .no_skip_triangles = nir_inot(b, nir_test_mask(b, args->flags, SpvRayFlagsSkipTrianglesKHRMask)),
      .no_skip_aabbs = nir_inot(b, nir_test_mask(b, args->flags, SpvRayFlagsSkipAABBsKHRMask)),
   };
   nir_loop *loop = nir_push_loop(b);
   nir_loop_add_continue_construct(loop);
   nir_push_if(b, nir_ieq_imm(b, nir_load_deref(b, args->vars.current_node), 0));
   {
      nir_push_if(b, nir_load_deref(b, args->vars.in_blas));
      {
         nir_store_deref(b, args->vars.current_node, nir_load_deref(b, args->vars.tlas_resume), 1);
         nir_store_deref(b, args->vars.in_blas, nir_imm_false(b), 1);
         nir_store_deref(b, args->vars.bvh_base, args->root_bvh_base, 1);
         nir_store_deref(b, args->vars.origin, args->origin, 7);
         nir_store_deref(b, args->vars.dir, args->dir, 7);
         nir_store_deref(b, args->vars.inv_dir, nir_fdiv(b, ones, args->dir), 7);
         nir_store_deref(b, args->vars.instance_addr, nir_imm_int64(b, 0), 1);
         nir_store_deref(b, args->vars.instance_payload_addr, nir_imm_int64(b, 0), 1);
         nir_store_deref(b, args->vars.sbt_offset_and_flags, nir_imm_int(b, 0), 1);
         nir_jump(b, nir_jump_continue);
      }
      nir_pop_if(b, NULL);
      nir_store_var(b, incomplete, nir_imm_false(b), 1);
      nir_jump(b, nir_jump_break);
   }
   nir_pop_if(b, NULL);
   nir_def *node = nir_iadd(b, nir_load_deref(b, args->vars.bvh_base),
                                 nir_load_deref(b, args->vars.current_node));
   nir_store_deref(b, args->vars.current_node,
      kk_rt_load(b, node, offsetof(struct kk_as_node, escape), 1, 64), 1);
   nir_push_if(b, nir_inot(b, kk_rt_box_intersects(b, node, args)));
   nir_jump(b, nir_jump_continue);
   nir_pop_if(b, NULL);
   nir_def *type = kk_rt_load(b, node, offsetof(struct kk_as_node, type), 1, 32);
   nir_push_if(b, nir_ieq_imm(b, type, KK_AS_NODE_INTERNAL));
   {
      nir_store_deref(b, args->vars.current_node,
         kk_rt_load(b, node, offsetof(struct kk_as_node, child0_or_payload), 1, 64), 1);
      nir_jump(b, nir_jump_continue);
   }
   nir_pop_if(b, NULL);
   nir_push_if(b, nir_ieq_imm(b, type, KK_AS_NODE_INSTANCE));
   {
      nir_def *mask = kk_rt_load(b, node, offsetof(struct kk_as_node, custom_index_and_mask), 1, 32);
      nir_def *blas = kk_rt_load(b, node, offsetof(struct kk_as_node, child1_or_blas), 1, 64);
      nir_push_if(b, nir_iand(b, nir_ine_imm(b, blas, 0),
                                  nir_ine_imm(b, nir_iand(b, mask, args->cull_mask), 0)));
      {
         nir_def *payload = nir_iadd(b, nir_load_deref(b, args->vars.bvh_base),
            kk_rt_load(b, node, offsetof(struct kk_as_node, child0_or_payload), 1, 64));
         nir_store_deref(b, args->vars.tlas_resume, nir_load_deref(b, args->vars.current_node), 1);
         nir_store_deref(b, args->vars.in_blas, nir_imm_true(b), 1);
         nir_store_deref(b, args->vars.instance_addr, node, 1);
         nir_store_deref(b, args->vars.instance_payload_addr, payload, 1);
         nir_store_deref(b, args->vars.sbt_offset_and_flags,
            kk_rt_load(b, node, offsetof(struct kk_as_node, sbt_offset_and_flags), 1, 32), 1);
         nir_store_deref(b, args->vars.bvh_base, blas, 1);
         nir_store_deref(b, args->vars.current_node,
            kk_rt_load(b, blas, offsetof(struct kk_as_header, root_offset), 1, 64), 1);
         nir_def *matrix[3];kk_rt_load_wto_matrix(b, payload, NULL, matrix);
         nir_store_deref(b, args->vars.origin, kk_rt_mul_vec3_mat(b, args->origin, matrix, true), 7);
         nir_store_deref(b, args->vars.dir, kk_rt_mul_vec3_mat(b, args->dir, matrix, false), 7);
         nir_store_deref(b, args->vars.inv_dir, nir_fdiv(b, ones, nir_load_deref(b, args->vars.dir)), 7);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
   nir_push_if(b, nir_ieq_imm(b, type, KK_AS_NODE_TRIANGLE));
   kk_rt_triangle(b, node, args, &flags);
   nir_pop_if(b, NULL);
   nir_push_if(b, nir_ieq_imm(b, type, KK_AS_NODE_AABB));
   kk_rt_aabb(b, node, args, &flags);
   nir_pop_if(b, NULL);
   nir_pop_loop(b, loop);
   nir_def *result = nir_load_var(b, incomplete);
   b->fp_math_ctrl = saved_math;
   return result;
}
