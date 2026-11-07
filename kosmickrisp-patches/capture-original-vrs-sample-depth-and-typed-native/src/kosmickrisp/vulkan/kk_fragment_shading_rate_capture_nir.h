/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_CAPTURE_NIR_H
#define KK_FRAGMENT_SHADING_RATE_CAPTURE_NIR_H
#include "kk_fragment_shading_rate_inputs_nir.h"
#include "kk_interlock_render_nir.h"
#include "kosmickrisp/libkk/kk_fragment_shading_rate_types.h"

/* The original quad planes serve interpolation. Covered fine samples have
 * their own exact native Z/W bank, independent of interpolation/extrapolation.
 * All four possible covered anchors carry the same fine-sample quad bank. */
static unsigned
kk_vrs_capture_fine_zw_offset(const struct kk_interlock_capture_nir *layout)
{
   return layout->stride + 32u + (layout->point_coord ? 32u : 0u);
}

static unsigned
kk_vrs_capture_input_stride(const struct kk_interlock_capture_nir *layout)
{
   return ALIGN_POT(kk_vrs_capture_fine_zw_offset(layout) + 32u, 16);
}

static nir_def *
kk_vrs_capture_parameters(nir_builder *b)
{
   return kk_interlock_env_load(b, nir_load_per_draw_ptr_kk(b, 1, 64), 1, 64,
      offsetof(struct kk_vrs_transaction_context, envelope));
}

static nir_def *
kk_vrs_capture_window(nir_builder *b, nir_def *env, nir_def *pixel)
{
   nir_def *local = nir_isub(b, pixel, nir_vec2(b,
      kk_interlock_env(b, env, tile_x), kk_interlock_env(b, env, tile_y)));
   return nir_iand(b,
      nir_ult(b, nir_channel(b, local, 0), kk_interlock_env(b, env, tile_width)),
      nir_ult(b, nir_channel(b, local, 1), kk_interlock_env(b, env, tile_height)));
}

/* One original job's retained raster reader supplies geometry and native
 * interpolation. The caller supplies its actual API identity in the existing
 * context and its actual logical Layer/ViewIndex in the immutable envelope.
 * Those values are not inferred from native expansion-triangle/job indices.
 * All commands reading this source must stay inside its producer callback. */
static nir_shader *
kk_vrs_capture_shader(const nir_shader_compiler_options *options,
   const struct kk_interlock_capture_nir *layout, unsigned samples)
{
   if (!layout || (samples != 1 && samples != 2 && samples != 4))
      return NULL;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      options, "VRS original sample and input capture");
   b.shader->info.io_lowered = true;
   b.shader->info.fs.uses_sample_shading = true;
   nir_def *context = nir_load_per_draw_ptr_kk(&b, 1, 64);
   nir_def *env = kk_vrs_capture_parameters(&b);
   nir_def *storage = kk_interlock_env_load(&b, context, 1, 32,
      offsetof(struct kk_vrs_transaction_context, capture_primitive));
   nir_def *coord = nir_load_frag_coord(&b);
   nir_def *pixel = nir_f2u32(&b, nir_channels(&b, coord, 3));
   nir_def *sample = nir_load_sample_id(&b);
   nir_def *geometric = nir_iand(&b, nir_load_sample_mask_in(&b),
      nir_ishl(&b, nir_imm_int(&b, 1), sample));
   nir_def *eligible = nir_iand(&b, kk_vrs_capture_window(&b, env, pixel),
      nir_iand(&b, nir_ult(&b, storage, kk_interlock_env(&b, env, primitive_count)),
         nir_iand(&b, nir_ult_imm(&b, sample, samples),
            nir_ine_imm(&b, geometric, 0))));
   nir_def *cell = kk_interlock_cell(&b, env, pixel, storage);
   nir_def *live = nir_iand(&b, eligible, nir_inot(&b, nir_is_helper_invocation(&b, 1)));
   nir_variable *previous = nir_local_variable_create(b.impl, glsl_uint_type(), NULL);
   nir_store_var(&b, previous, nir_imm_int(&b, ~0u), 1);
   nir_push_if(&b, live);
   nir_store_var(&b, previous, nir_global_atomic(&b, 32,
      kk_interlock_word_address(&b, kk_interlock_env(&b, env, claims), cell),
      geometric, .atomic_op = nir_atomic_op_ior), 1);
   nir_global_atomic(&b, 32,
      kk_interlock_word_address(&b, kk_interlock_env(&b, env, coverage), cell),
      geometric, .atomic_op = nir_atomic_op_ior);
   nir_pop_if(&b, NULL);
   nir_def *won = nir_iand(&b, live, nir_ieq_imm(&b, nir_load_var(&b, previous), 0));
   nir_def *fine_won = nir_iand(&b, live,
      nir_ieq_imm(&b, nir_iand(&b, nir_load_var(&b, previous), geometric), 0));

   /* Reconstruct one coherent sample-zero plane for interpolation. The exact
    * fine values below never use these derivative/extrapolated coordinates. */
   nir_def *zw = nir_channels(&b, coord, 12);
   nir_def *offset = nir_fsub(&b, nir_load_sample_pos_from_id(&b, 32, nir_imm_int(&b, 0)),
      nir_load_sample_pos(&b));
   nir_def *sample_zero = nir_ffma(&b, nir_ddy(&b, zw), nir_channel(&b, offset, 1),
      nir_ffma(&b, nir_ddx(&b, zw), nir_channel(&b, offset, 0), zw));
   struct kk_interlock_capture_record_nir record = {env, cell};
   kk_interlock_capture_samples(&b, layout, samples, kk_interlock_env(&b, env, samples),
      won, sample_zero, &record, kk_interlock_capture_record);

   /* Any covered pixel may become this quad's anchor. Store each actual
    * pixel/sample's Z/W in every possible anchor, at disjoint lane offsets.
    * Each sample claims its lane once, including overlapping native expansion
    * triangles. No application body, helper write or second DS test runs. */
   nir_def *quad = nir_iand_imm(&b, pixel, ~1u);
   nir_def *lane = nir_iadd(&b, nir_iand_imm(&b, nir_channel(&b, pixel, 0), 1),
      nir_ishl_imm(&b, nir_iand_imm(&b, nir_channel(&b, pixel, 1), 1), 1));
   nir_push_if(&b, fine_won);
   for (unsigned anchor = 0; anchor < 4; anchor++) {
      nir_def *point = nir_iadd(&b, quad, nir_imm_ivec2(&b, anchor & 1, anchor >> 1));
      nir_push_if(&b, kk_vrs_capture_window(&b, env, point));
      nir_def *target = kk_interlock_record(&b, env,
         kk_interlock_cell(&b, env, point, storage), sample);
      target = nir_iadd(&b, nir_iadd_imm(&b, target, kk_vrs_capture_fine_zw_offset(layout)),
         nir_u2u64(&b, nir_imul_imm(&b, lane, 8)));
      nir_store_global(&b, zw, target, .write_mask = 3, .align_mul = 8);
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);
   nir_push_if(&b, won);
   nir_def *metadata = nir_iadd(&b, kk_interlock_env(&b, env, metadata),
      nir_u2u64(&b, nir_imul_imm(&b, cell, 16)));
   nir_store_global(&b, nir_vec4(&b,
      kk_interlock_env_load(&b, context, 1, 32, offsetof(struct kk_vrs_context, api_primitive)),
      nir_b2i32(&b, nir_ior(&b,
         nir_ult_imm(&b, kk_interlock_source_arity(&b, env), 3), nir_load_front_face(&b, 1))),
      kk_interlock_env(&b, env, layer), kk_interlock_env(&b, env, source_view)),
      metadata, .write_mask = 15, .align_mul = 16);
   nir_pop_if(&b, NULL);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}
#endif
