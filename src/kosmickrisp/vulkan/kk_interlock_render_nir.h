/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_RENDER_NIR_H
#define KK_INTERLOCK_RENDER_NIR_H

#include "kk_interlock_envelope.h"
#include "kk_interlock_capture_nir.h"
#include "kk_interlock_program_nir.h"

/* Parameters are read through the existing Poly capture pointer in production.
 * The callback also lets native compiler probes supply a standalone parameter
 * buffer, without a second shader emitter or a second parameter declaration. */
struct kk_interlock_render_nir {
   struct kk_interlock_capture_nir inputs;
   /* Application frequency is saved before fixed-function output lowering. */
   bool sample_shading;
   void (*lower_sample_outputs)(nir_shader *, void *);
   void *sample_outputs_data;
   nir_def *(*parameters)(nir_builder *, void *);
   void *data;
   const struct poly_gs_capture_info *geometry;
   nir_def *(*point_coord)(nir_builder *, const struct poly_gs_capture_info *,
      nir_def *params, nir_def *primitive, nir_def *pixel_center);
};

static nir_def *
kk_interlock_env_load(nir_builder *b, nir_def *params,
                       unsigned components, unsigned bits, unsigned offset)
{
   return nir_load_global_constant(b, components, bits,
      nir_iadd_imm(b, params, offset), .align_mul = bits / 8);
}

#define kk_interlock_env(b, params, field) \
   kk_interlock_env_load(b, params, 1, \
      8 * sizeof(((struct kk_interlock_envelope *)0)->field), \
      offsetof(struct kk_interlock_envelope, field))

static nir_def *
kk_interlock_source_arity(nir_builder *b, nir_def *params)
{
   nir_def *arity = kk_interlock_env(b, params, source_vertices_per_primitive);
   return nir_bcsel(b, nir_ine_imm(b, arity, 0), arity, nir_imm_int(b, 3));
}

static nir_def *
kk_interlock_cell(nir_builder *b, nir_def *params,
                    nir_def *pixel, nir_def *primitive)
{
   nir_def *x = nir_isub(b, nir_channel(b, pixel, 0),
                         kk_interlock_env(b, params, tile_x));
   nir_def *y = nir_isub(b, nir_channel(b, pixel, 1),
                         kk_interlock_env(b, params, tile_y));
   nir_def *height = kk_interlock_env(b, params, tile_height);
   nir_def *width = kk_interlock_env(b, params, tile_width);
   return nir_iadd(b, nir_imul(b,
      nir_iadd(b, nir_imul(b, primitive, height), y), width), x);
}

static nir_def *
kk_interlock_word_address(nir_builder *b, nir_def *base, nir_def *index)
{
   return nir_iadd(b, base, nir_u2u64(b, nir_imul_imm(b, index, 4)));
}

struct kk_interlock_capture_record_nir {
   nir_def *params;
   nir_def *cell;
};

static nir_def *
kk_interlock_record(nir_builder *b, nir_def *params,
                      nir_def *cell, nir_def *sample)
{
   nir_def *index = nir_iadd(b, nir_imul(b, cell,
      kk_interlock_env(b, params, samples)), sample);
   nir_def *bytes = nir_u2u64(b, nir_imul(b, index,
      kk_interlock_env(b, params, input_stride)));
   return nir_iadd(b, kk_interlock_env(b, params, inputs), bytes);
}

static nir_def *
kk_interlock_capture_record(nir_builder *b, void *data, unsigned sample)
{
   struct kk_interlock_capture_record_nir *r = data;
   return kk_interlock_record(b, r->params, r->cell, nir_imm_int(b, sample));
}

/* The explicitly clipped output retains the application primitive ID. Native
 * primitive_id instead numbers the bounded raster batch's clipped triangles. */
static nir_def *
kk_interlock_application_primitive(nir_builder *b,
   struct kk_interlock_render_nir *s, nir_def *params, nir_def *primitive)
{
   nir_def *global = nir_iadd(b, primitive,
      kk_interlock_env(b, params, primitive_base));
   if (!s->geometry || !(s->geometry->outputs & VARYING_BIT_PRIMITIVE_ID))
      return global;
   unsigned slots = util_bitcount64(s->geometry->outputs);
   unsigned slot = util_bitcount64(s->geometry->outputs &
      BITFIELD64_MASK(VARYING_SLOT_PRIMITIVE_ID));
   nir_def *vertex = nir_bcsel(b,
      nir_ine_imm(b, kk_interlock_env(b, params, source_points_expanded), 0),
      nir_ushr_imm(b, global, 1),
      nir_imul(b, global, kk_interlock_source_arity(b, params)));
   nir_def *bytes = nir_imul_imm(b, vertex, (slots + 1) * 16);
   nir_def *output = kk_interlock_env_load(b, params, 1, 64,
      offsetof(struct kk_interlock_envelope, geometry.output));
   return nir_load_global_constant(b, 1, 32,
      nir_iadd(b, output, nir_u2u64(b, nir_iadd_imm(b, bytes, slot * 16))),
      .align_mul = 16);
}

/* The renderer scissors to an aligned tile and draws one bounded batch of
 * already clipped raster primitives. Every covered sample contributes raw
 * coverage; no final depth/stencil state is used as a capture prefilter.
 * The winning physical sample captures all interpolation planes, including
 * original helper values. Losing samples retain coverage and run no source
 * application code. */
static nir_builder
kk_interlock_capture_shader(const nir_shader_compiler_options *options,
                              struct kk_interlock_render_nir *s,
                              unsigned max_samples, nir_def **winner)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      options, "interlock native input capture");
   b.shader->info.io_lowered = true;
   b.shader->info.fs.uses_sample_shading = true;
   nir_def *params = s->parameters(&b, s->data);
   nir_def *coord = nir_load_frag_coord(&b);
   nir_def *pixel = nir_f2u32(&b, nir_channels(&b, coord, 3));
   nir_def *primitive = nir_load_primitive_id(&b);
   nir_def *layer = nir_load_layer_id(&b);
   nir_def *selected_layer = nir_ieq(&b, layer,
      kk_interlock_env(&b, params, layer));
   nir_def *cell = kk_interlock_cell(&b, params, pixel, primitive);
   nir_def *claim = kk_interlock_word_address(&b,
      kk_interlock_env(&b, params, claims), cell);
   nir_def *won = kk_interlock_capture_claim_if(&b, claim, selected_layer);
   nir_push_if(&b, nir_iand(&b, selected_layer,
      nir_inot(&b, nir_is_helper_invocation(&b, 1))));
   nir_def *coverage = kk_interlock_word_address(&b,
      kk_interlock_env(&b, params, coverage), cell);
   nir_global_atomic(&b, 32, coverage,
      nir_ishl(&b, nir_imm_int(&b, 1), nir_load_sample_id(&b)),
      .atomic_op = nir_atomic_op_ior);
   nir_pop_if(&b, NULL);
   struct kk_interlock_capture_record_nir record = {params, cell};
   kk_interlock_capture_samples(&b, &s->inputs, max_samples,
      kk_interlock_env(&b, params, samples), won, nir_channels(&b, coord, 12),
      &record, kk_interlock_capture_record);
   nir_push_if(&b, won);
   nir_def *metadata = nir_iadd(&b,
      kk_interlock_env(&b, params, metadata),
      nir_u2u64(&b, nir_imul_imm(&b, cell, 16)));
   nir_store_global(&b, nir_vec4(&b,
      kk_interlock_application_primitive(&b, s, params, primitive),
      nir_b2i32(&b, nir_ior(&b,
         nir_ult_imm(&b, kk_interlock_source_arity(&b, params), 3),
         nir_load_front_face(&b, 1))), layer,
      nir_bcsel(&b, nir_ine_imm(&b,
         kk_interlock_env(&b, params, view_mask), 0), layer,
         nir_imm_int(&b, 0))), metadata, .align_mul = 16);
   nir_pop_if(&b, NULL);
   if (winner)
      *winner = won;
   return b;
}

struct kk_interlock_transaction_nir {
   struct kk_interlock_render_nir *render;
   nir_variable *anchor;
   nir_variable *pixel;
   nir_variable *primitive;
   nir_variable *coverage;
   nir_variable *metadata;
   void *data;
};

/* Every covered pixel captured its entire original quad. For a virtual helper
 * choose any covered pixel in that same quad; its record includes the helper's
 * own lane. Tile origins and extents are even, so the quad is never split. */
static nir_def *
kk_interlock_quad_anchor(nir_builder *b, nir_def *params,
                           nir_def *pixel, nir_def *primitive)
{
   nir_def *origin = nir_iand_imm(b, pixel, ~1u);
   nir_def *anchor = kk_interlock_cell(b, params, origin, primitive);
   nir_def *found = nir_imm_false(b);
   for (unsigned lane = 0; lane < 4; ++lane) {
      nir_def *point = nir_iadd(b, origin,
         nir_imm_ivec2(b, lane & 1u, lane >> 1u));
      nir_def *cell = kk_interlock_cell(b, params, point, primitive);
      nir_def *coverage = nir_load_global_constant(b, 1, 32,
         kk_interlock_word_address(b,
            kk_interlock_env(b, params, coverage), cell), .align_mul = 4);
      nir_def *valid = nir_ine_imm(b, coverage, 0);
      anchor = nir_bcsel(b, nir_iand(b, valid, nir_inot(b, found)), cell, anchor);
      found = nir_ior(b, found, valid);
   }
   return anchor;
}

static nir_def *
kk_interlock_transaction_record(nir_builder *b, void *data, nir_def *sample)
{
   struct kk_interlock_transaction_nir *s = data;
   nir_def *params = s->render->parameters(b, s->render->data);
   return kk_interlock_record(b, params, nir_load_var(b, s->anchor), sample);
}

static nir_def *
kk_interlock_captured_sample_position(nir_builder *b, void *data, nir_def *sample)
{
   struct kk_interlock_transaction_nir *s = data;
   nir_def *params = s->render->parameters(b, s->render->data);
   nir_def *offset = nir_u2u64(b, nir_imul_imm(b, sample, 2 * sizeof(float)));
   return nir_load_global_constant(b, 2, 32,
      nir_iadd(b, nir_iadd_imm(b, params,
         offsetof(struct kk_interlock_envelope, sample_positions)), offset),
      .align_mul = sizeof(float));
}

static void
kk_interlock_transaction_inputs(nir_shader *shader,
                                  struct kk_interlock_program_nir *program)
{
   struct kk_interlock_transaction_nir *s = program->data;
   struct kk_interlock_captured_inputs_nir inputs = {
      .capture = &s->render->inputs,
      .sample = program->sample,
      .coord = program->coord,
      .data = s,
      .record = kk_interlock_transaction_record,
      .sample_position = kk_interlock_captured_sample_position,
      .sample_shading = program->sample_shading,
   };
   kk_interlock_lower_captured_inputs(shader, &inputs);
}

static nir_def *
kk_interlock_transaction_samples(nir_builder *b,
                                   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_transaction_nir *s = program->data;
   return kk_interlock_env(b,
      s->render->parameters(b, s->render->data), samples);
}

static nir_def *
kk_interlock_transaction_sample_position(nir_builder *b,
   struct kk_interlock_program_nir *program, nir_def *sample)
{
   return kk_interlock_captured_sample_position(b, program->data, sample);
}

static void
kk_interlock_transaction_begin_pixel(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_transaction_nir *s = program->data;
   nir_def *params = s->render->parameters(b, s->render->data);
   nir_def *pixel = nir_f2u32(b, nir_channels(b, nir_load_frag_coord(b), 3));
   nir_def *primitive = nir_load_primitive_id(b);
   nir_store_var(b, s->pixel, pixel, 3);
   nir_store_var(b, s->primitive, primitive, 1);
   nir_def *anchor = kk_interlock_quad_anchor(b, params, pixel, primitive);
   nir_store_var(b, s->anchor, anchor, 1);
   nir_def *cell = kk_interlock_cell(b, params, pixel, primitive);
   nir_def *mask = nir_load_global_constant(b, 1, 32,
      kk_interlock_word_address(b,
         kk_interlock_env(b, params, coverage), cell), .align_mul = 4);
   nir_store_var(b, s->coverage, mask, 1);
   nir_def *metadata = nir_load_global_constant(b, 4, 32,
      nir_iadd(b, kk_interlock_env(b, params, metadata),
         nir_u2u64(b, nir_imul_imm(b, anchor, 16))), .align_mul = 16);
   nir_store_var(b, s->metadata, metadata, 15);
}

static nir_def *
kk_interlock_transaction_active_quad(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_transaction_nir *s = program->data;
   return nir_quad_vote_any(b, 1,
      nir_ine_imm(b, nir_load_var(b, s->coverage), 0));
}

static void
kk_interlock_transaction_begin_sample(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_transaction_nir *s = program->data;
   nir_def *pixel = nir_load_var(b, s->pixel);
   nir_def *sample = nir_load_var(b, program->sample);
   nir_def *record = kk_interlock_transaction_record(b, s, sample);
   nir_def *zw = kk_interlock_quad_load(b,
      nir_iadd_imm(b, record, s->render->inputs.stride), 8, 2, 32, pixel);
   if (!program->sample_shading) {
      /* Restore pixel-center depth/W with the same captured affine planes
       * used for center-qualified inputs, including original helper lanes. */
      nir_def *base = kk_interlock_record(b,
         s->render->parameters(b, s->render->data),
         nir_load_var(b, s->anchor), nir_imm_int(b, 0));
      nir_def *base_offset = nir_fadd_imm(b,
         kk_interlock_captured_sample_position(b, s, nir_imm_int(b, 0)), -0.5f);
      zw = kk_interlock_captured_offset(b, base,
         nir_iadd_imm(b, base, s->render->inputs.stride), &s->render->inputs,
         8, 2, pixel, nir_imm_vec2(b, 0, 0), base_offset, false);
   }
   nir_def *center = nir_fadd_imm(b, nir_u2f32(b, pixel), 0.5f);
   nir_def *point_coord = s->render->point_coord ? s->render->point_coord(b,
      s->render->geometry, s->render->parameters(b, s->render->data),
      nir_iadd(b, nir_load_var(b, s->primitive), kk_interlock_env(b,
         s->render->parameters(b, s->render->data), primitive_base)), center) :
      nir_imm_vec2(b, 0.5f, 0.5f);
   nir_store_var(b, program->point_coord, point_coord, 3);
   nir_store_var(b, program->coord, nir_vec4(b,
      nir_channel(b, center, 0), nir_channel(b, center, 1),
      nir_channel(b, zw, 0), nir_channel(b, zw, 1)), 15);
   nir_store_var(b, program->sample_position,
      kk_interlock_transaction_sample_position(b, program, sample), 3);
   nir_def *metadata = nir_load_var(b, s->metadata);
   nir_store_var(b, program->primitive, nir_channel(b, metadata, 0), 1);
   nir_store_var(b, program->front_face,
      nir_ine_imm(b, nir_channel(b, metadata, 1), 0), 1);
   nir_store_var(b, program->layer, nir_channel(b, metadata, 2), 1);
   nir_store_var(b, program->view_index, nir_channel(b, metadata, 3), 1);
   nir_def *mask = nir_load_var(b, s->coverage);
   nir_store_var(b, program->coverage, mask, 1);
   nir_def *covered = nir_ine_imm(b, program->sample_shading ?
      nir_iand(b, mask, nir_ishl(b, nir_imm_int(b, 1), sample)) : mask, 0);
   nir_def *helper = nir_ior(b, nir_inot(b, covered),
      nir_is_helper_invocation(b, 1));
   nir_store_var(b, program->initial_helper, helper, 1);
   nir_store_var(b, program->helper, helper, 1);
}

/* Call before wrapping, while source NIR is still the original entry point.
 * Additional transaction callbacks can extend begin/end-pixel and end-sample
 * for ordered shadow tests, color updates and sample counting. */
static void
kk_interlock_transaction_init(nir_shader *shader,
   struct kk_interlock_transaction_nir *s,
   struct kk_interlock_program_nir *program)
{
   s->anchor = kk_interlock_program_var(shader, glsl_uint_type(), "quad_record");
   s->pixel = kk_interlock_program_var(shader, glsl_uvec2_type(), "native_pixel");
   s->primitive = kk_interlock_program_var(shader, glsl_uint_type(), "raster_primitive");
   s->coverage = kk_interlock_program_var(shader, glsl_uint_type(), "raster_coverage");
   s->metadata = kk_interlock_program_var(shader, glsl_uvec4_type(), "source_metadata");
   program->sample_shading = s->render->sample_shading;
   program->data = s;
   program->lower_inputs = kk_interlock_transaction_inputs;
   program->sample_count = kk_interlock_transaction_samples;
   program->sample_position_for_id = kk_interlock_transaction_sample_position;
   program->begin_pixel = kk_interlock_transaction_begin_pixel;
   program->active_quad = kk_interlock_transaction_active_quad;
   program->begin_sample = kk_interlock_transaction_begin_sample;
}

#endif
