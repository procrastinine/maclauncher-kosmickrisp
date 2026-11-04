/*
 * Copyright 2016 Broadcom
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * Texture source handling follows Mesa nir_lower_tex.c at 4f071f891d63.
 * Linear quad mapping follows the SPV_KHR_compute_shader_derivatives contract;
 * the MetalSharp reference uses the same four consecutive Metal SIMD lanes.
 */
#ifndef MSL_COMPUTE_DERIVATIVES_H
#define MSL_COMPUTE_DERIVATIVES_H
#include "nir.h"
#include "kk-sampler-helper-flags.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"
#include "util/set.h"
#include "util/u_dynarray.h"

static inline bool
kk_lower_compute_derivative(nir_builder *b, nir_intrinsic_instr *intr,
                            UNUSED void *data)
{
   bool y, coarse = false;
   switch (intr->intrinsic) {
   case nir_intrinsic_ddx_coarse: coarse = true; FALLTHROUGH;
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine: y = false; break;
   case nir_intrinsic_ddy_coarse: coarse = true; FALLTHROUGH;
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine: y = true; break;
   default: return false;
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *lane = nir_load_subgroup_invocation(b);
   nir_def *lo = coarse ? nir_imm_int(b, 0) :
      nir_iand_imm(b, lane, y ? 1 : 2);
   nir_def *hi = nir_ior_imm(b, lo, y ? 2 : 1);
   nir_def *v = intr->src[0].ssa;
   nir_def_replace(&intr->def, nir_fsub(b, nir_quad_broadcast(b, v, hi),
                                          nir_quad_broadcast(b, v, lo)));
   return true;
}

static inline bool
kk_compute_texture_gradients(nir_builder *b, nir_tex_instr *tex, UNUSED void *data)
{
   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb)
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   if (tex->is_array)
      coord = nir_trim_vector(b, coord, coord->num_components - 1);
   nir_def *dx = nir_ddx(b, coord), *dy = nir_ddy(b, coord);
   int bias = nir_tex_instr_src_index(tex, nir_tex_src_bias);
   if (bias >= 0) {
      nir_def *scale = nir_fexp2(b, nir_f2f32(b, tex->src[bias].src.ssa));
      dx = nir_fmul(b, dx, scale);
      dy = nir_fmul(b, dy, scale);
      nir_tex_instr_remove_src(tex, bias);
   }
   tex->op = nir_texop_txd;
   nir_tex_instr_add_src(tex, nir_tex_src_ddx, dx);
   nir_tex_instr_add_src(tex, nir_tex_src_ddy, dy);
   return true;
}

/* Called only after the entry point checks the derivative execution mode. */
static inline void
kk_compute_derivative_textures(nir_shader *nir)
{
   /* Project before differentiating. The normal texture pass subsequently
    * handles 1D, cube gradients and descriptor sampler LOD bias. */
   const nir_lower_tex_options project = {.lower_txp = ~0u};
   nir_lower_tex(nir, &project);
   nir_shader_tex_pass(nir, kk_compute_texture_gradients,
                       nir_metadata_control_flow, NULL);
}

static inline bool
kk_lod_probe(const nir_tex_instr *tex, const void *probes)
{
   return _mesa_set_search(probes, tex) != NULL;
}

static inline void
kk_compute_derivative_lod(nir_shader *nir)
{
   struct kk_lod_query {
      nir_tex_instr *query, *probe;
   } stack_queries[16];
   struct util_dynarray queries;
   util_dynarray_init_from_stack(&queries, stack_queries, sizeof(stack_queries));
   struct set probes;
   _mesa_pointer_set_init(&probes, NULL);

   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      bool progress = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->op != nir_texop_lod || (tex->backend_flags & KK_SAMPLE_HELPER))
               continue;
            b.cursor = nir_before_instr(instr);
            nir_tex_instr *probe = nir_instr_as_tex(nir_instr_clone(nir, &tex->instr));
            probe->op = nir_texop_tex;
            nir_builder_instr_insert(&b, &probe->instr);
            for (int i = probe->num_srcs - 1; i >= 0; --i)
               if (probe->src[i].src_type == nir_tex_src_min_lod ||
                   probe->src[i].src_type == nir_tex_src_max_lod_kk ||
                   probe->src[i].src_type == nir_tex_src_bias ||
                   probe->src[i].src_type == nir_tex_src_backend1)
                  nir_tex_instr_remove_src(probe, i);
            kk_compute_texture_gradients(&b, probe, NULL);
            _mesa_set_add(&probes, probe);
            struct kk_lod_query query = {tex, probe};
            util_dynarray_append(&queries, query);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }

   if (probes.entries) {
      /* Reuse Mesa's dimension-aware rho and cube quotient-rule math in one
       * shader pass for all queries. Real samples keep their gradients and
       * therefore retain anisotropic filtering. */
      const nir_lower_tex_options options = {
         .lower_txd_cb = kk_lod_probe, .lower_txd_data = &probes,
      };
      nir_lower_tex(nir, &options);
      util_dynarray_foreach(&queries, struct kk_lod_query, query) {
         nir_tex_instr *tex = query->query, *probe = query->probe;
         nir_builder b = nir_builder_at(nir_before_instr(&tex->instr));
         /* The descriptor pass supplied these bounds for the query. */
         nir_def *min = nir_get_tex_src(tex, nir_tex_src_min_lod);
         nir_def *max = nir_get_tex_src(tex, nir_tex_src_max_lod_kk);
         nir_def *bias_src = nir_get_tex_src(tex, nir_tex_src_bias);
         nir_def *bias = bias_src ? bias_src : nir_imm_float(&b, 0);
         nir_def *metadata = nir_get_tex_src(tex, nir_tex_src_backend1);
         nir_def *mode = nir_imm_int(&b, 1);
         if (metadata) {
            bias = nir_f2f32(&b, nir_u2u16(&b, nir_channel(&b, metadata, 0)));
            mode = nir_channel(&b, metadata, 1);
         }
         nir_def *lod = nir_get_tex_src(probe, nir_tex_src_lod);
         nir_def *unclamped = nir_fadd(&b, lod, bias);
         nir_def *clamped = nir_fmin(&b, nir_fmax(&b, unclamped, min), max);
         nir_def *levels = nir_build_texture_query(&b, tex,
            nir_texop_query_levels, 1, nir_type_uint32, false, false);
         clamped = nir_fmin(&b, nir_fmax(&b, clamped, nir_imm_float(&b, 0)),
                            nir_u2f32(&b, nir_iadd_imm(&b, levels, -1)));
         nir_def *nearest = nir_fadd_imm(&b,
            nir_fceil(&b, nir_fadd_imm(&b, clamped, 0.5)), -1.0);
         clamped = nir_bcsel(&b, nir_ieq_imm(&b, mode, 0), nearest, clamped);
         nir_def_replace(&tex->def, nir_vec2(&b, clamped, unclamped));
         nir_instr_remove(&probe->instr);
         nir_progress(true, b.impl, nir_metadata_control_flow);
      }
   }
   _mesa_set_fini(&probes, NULL);
   util_dynarray_fini(&queries);
}
static bool
kk_unhandled_cube_gradient(const nir_tex_instr *tex, UNUSED const void *data)
{
   return tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE &&
          !(tex->backend_flags & KK_SAMPLE_HELPER);
}

static inline void
kk_lower_compute_derivatives(nir_shader *nir)
{
   if (nir->info.derivative_group != DERIVATIVE_GROUP_LINEAR ||
       nir->info.stage != MESA_SHADER_COMPUTE)
      return;
   kk_compute_derivative_textures(nir);
   const nir_lower_tex_options cubes = {.lower_txd_cb = kk_unhandled_cube_gradient};
   nir_lower_tex(nir, &cubes);
   kk_compute_derivative_lod(nir);
   nir_shader_intrinsics_pass(nir, kk_lower_compute_derivative,
                              nir_metadata_control_flow, NULL);
}
#endif
