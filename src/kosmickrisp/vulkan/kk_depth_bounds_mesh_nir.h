/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BOUNDS_MESH_NIR_H
#define KK_DEPTH_BOUNDS_MESH_NIR_H
#include "kk_depth_bounds.h"

static nir_def *
kk_depth_bounds_mesh_parameters(nir_builder *b, void *data)
{
   return nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, nir_load_per_draw_ptr_kk(b, 1, 64),
         offsetof(struct kk_depth_bounds_capture_context, env)), .align_mul = 8);
}

/* Capture runs on the original clipped primitive before the software depth
 * test. Bias its recorded depth rather than moving clip-space vertices or
 * changing application fragment outputs and early-test ordering. */
static bool
kk_depth_bounds_mesh_bias(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_frag_coord)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   nir_def *params = kk_depth_bounds_mesh_parameters(b, NULL);
#define mesh_bias_field(member) kk_interlock_env_load(b, params, 1, \
      8 * sizeof(((struct poly_geometry_capture *)0)->member), \
      offsetof(struct kk_interlock_envelope, geometry.member))
   nir_def *representation = mesh_bias_field(depth_bias_representation);
   nir_def *mantissa = nir_u2u32(b, mesh_bias_field(depth_mantissa_bits));
   nir_def *is_float = nir_u2u32(b, mesh_bias_field(depth_is_float));
   nir_def *power = nir_bcsel(b, nir_ieq_imm(b, representation, 1),
      nir_ineg(b, nir_iadd(b, mantissa, is_float)), nir_imm_int(b, 0));
   nir_def *unit = nir_ishl_imm(b, nir_iadd_imm(b, power, 127), 23);
   nir_def *z = nir_channel(b, &intr->def, 2);
   nir_def *slope = nir_fmax(b, nir_fabs(b, nir_ddx_fine(b, z)),
                              nir_fabs(b, nir_ddy_fine(b, z)));
   slope = nir_bcsel(b, nir_uge_imm(b,
      kk_interlock_env(b, params, source_vertices_per_primitive), 3),
      slope, nir_imm_float(b, 0));
   nir_def *bias = nir_fadd(b,
      nir_fmul(b, mesh_bias_field(depth_bias_constant), unit),
      nir_fmul(b, mesh_bias_field(depth_bias_slope), slope));
   nir_def *clamp = mesh_bias_field(depth_bias_clamp);
   bias = nir_bcsel(b, nir_flt_imm(b, clamp, 0), nir_fmax(b, bias, clamp),
      nir_bcsel(b, nir_fgt_imm(b, clamp, 0), nir_fmin(b, bias, clamp), bias));
   nir_def *active = nir_iand(b,
      nir_ine_imm(b, mesh_bias_field(depth_bias_enable), 0),
      nir_ine_imm(b, representation, 0));
   active = nir_iand(b, active, nir_ine_imm(b,
      kk_interlock_env(b, params, source_vertices_per_primitive), 2));
   nir_def *depth = nir_bcsel(b, active, nir_fadd(b, z, bias), z);
   nir_def *coord = nir_vec4(b, nir_channel(b, &intr->def, 0),
      nir_channel(b, &intr->def, 1), depth, nir_channel(b, &intr->def, 3));
   nir_def_rewrite_uses_after(&intr->def, coord);
   b->shader->info.fs.needs_full_quad_helper_invocations = true;
#undef mesh_bias_field
   return true;
}

/* The reader validates the authoritative GPU group/slot against this batch's
 * grid/counts. Storage identity chooses a cell; the shader's PrimitiveID stays
 * in its independently captured typed input and is never used as an address. */
static nir_def *
kk_depth_bounds_mesh_job(nir_builder *b, void *data)
{
   nir_def *context = nir_load_per_draw_ptr_kk(b, 1, 64);
   nir_def *job = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, context, offsetof(struct kk_depth_bounds_capture_context, job)),
      .align_mul = 8);
   nir_def *slot = nir_load_global_constant(b, 2, 32, job, .align_mul = 4);
   nir_def *maximum = nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, context,
         offsetof(struct kk_depth_bounds_capture_context, max_primitives)), .align_mul = 4);
   nir_def *identity = nir_iadd(b,
      nir_imul(b, nir_channel(b, slot, 0), maximum), nir_channel(b, slot, 1));
   return nir_isub(b, identity, kk_interlock_env(b,
      kk_depth_bounds_mesh_parameters(b, NULL), primitive_base));
}

static bool
kk_depth_bounds_mesh_original_per_draw(nir_builder *b,
   nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_per_draw_ptr_kk)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *original = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, nir_load_per_draw_ptr_kk(b, 1, 64),
         offsetof(struct kk_depth_bounds_capture_context, original_per_draw)),
      .align_mul = 8);
   nir_def_replace(&intr->def, original);
   return true;
}
#endif
