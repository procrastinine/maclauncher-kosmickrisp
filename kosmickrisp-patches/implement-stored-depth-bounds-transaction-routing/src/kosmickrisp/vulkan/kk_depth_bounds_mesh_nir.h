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
