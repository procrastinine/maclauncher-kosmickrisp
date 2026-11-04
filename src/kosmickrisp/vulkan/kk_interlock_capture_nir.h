/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_CAPTURE_NIR_H
#define KK_INTERLOCK_CAPTURE_NIR_H

#include "kk_interlock_quad_nir.h"

/* Coverage is recorded without depth/stencil tests. Those tests run against
 * the transaction's current shadow values, including earlier primitives in
 * this draw. Every buffer is bounded by the continuation's tile and batch. */
struct kk_interlock_capture_nir {
   struct kk_interlock_quad_layout layout[3];
   uint16_t kind_offset[3];
   uint16_t stride;
   nir_alu_type types[NUM_TOTAL_VARYING_SLOTS];
   uint8_t components[NUM_TOTAL_VARYING_SLOTS];
   uint8_t interpolation[NUM_TOTAL_VARYING_SLOTS];
   uint64_t per_primitive;
   bool point_coord, point_sample;
   uint64_t center;
   uint64_t centroid;
   uint64_t sample;
   uint64_t interpolants;
};

static bool
kk_interlock_capture_collect(nir_builder *b, nir_intrinsic_instr *intr,
                               void *data)
{
   struct kk_interlock_capture_nir *s = data;
   if (intr->intrinsic == nir_intrinsic_load_point_coord) {
      s->point_coord = true;
      return false;
   }
   bool interpolated = intr->intrinsic == nir_intrinsic_load_interpolated_input;
   if (!interpolated && intr->intrinsic != nir_intrinsic_load_input &&
       intr->intrinsic != nir_intrinsic_load_per_primitive_input)
      return false;
   nir_src offset = intr->src[interpolated ? 1 : 0];
   assert(nir_src_is_const(offset));
   unsigned slot = nir_intrinsic_io_semantics(intr).location +
                   nir_src_as_uint(offset);
   uint64_t bit = BITFIELD64_BIT(slot);
   s->types[slot] = nir_intrinsic_dest_type(intr);
   s->components[slot] = MAX2(s->components[slot],
      intr->num_components + nir_intrinsic_component(intr));
   if (!interpolated) {
      if (intr->intrinsic == nir_intrinsic_load_per_primitive_input)
         s->per_primitive |= bit;
      s->center |= bit;
      s->interpolation[slot] = INTERP_MODE_FLAT;
      return false;
   }
   nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
   s->interpolation[slot] = nir_intrinsic_interp_mode(bary);
   switch (bary->intrinsic) {
   case nir_intrinsic_load_barycentric_pixel:
      s->center |= bit;
      break;
   case nir_intrinsic_load_barycentric_centroid:
      s->centroid |= bit;
      break;
   case nir_intrinsic_load_barycentric_sample:
      s->sample |= bit;
      break;
   case nir_intrinsic_load_barycentric_at_sample:
      s->sample |= bit;
      s->interpolants |= bit;
      break;
   case nir_intrinsic_load_barycentric_at_offset:
      s->center |= bit;
      s->interpolants |= bit;
      break;
   default:
      UNREACHABLE("interlock input interpolation");
   }
   return false;
}

static void
kk_interlock_capture_layout(nir_shader *program,
                              struct kk_interlock_capture_nir *s)
{
   memset(s, 0, sizeof(*s));
   nir_shader_intrinsics_pass(program, kk_interlock_capture_collect,
                               nir_metadata_all, s);
   s->interpolants |= (s->center & s->centroid) |
                      (s->center & s->sample) | (s->centroid & s->sample);
   const uint64_t masks[] = {s->center, s->centroid, s->sample};
   for (unsigned kind = 0; kind < ARRAY_SIZE(masks); ++kind) {
      kk_interlock_quad_layout_init(&s->layout[kind], masks[kind],
                                      s->types, s->components);
      s->kind_offset[kind] = s->stride;
      s->stride += 4 * s->layout[kind].stride;
   }
}

static nir_def *
kk_interlock_capture_input(nir_builder *b,
                             const struct kk_interlock_capture_nir *s,
                             unsigned slot, unsigned kind, nir_def *sample)
{
   nir_io_semantics io = {.location = slot, .num_slots = 1};
   unsigned components = s->components[slot];
   nir_alu_type type = s->types[slot];
   unsigned bits = nir_alu_type_get_type_size(type);
   nir_def *value;
   if (s->per_primitive & BITFIELD64_BIT(slot)) {
      value = nir_load_per_primitive_input(b, components, bits, nir_imm_int(b, 0),
         .dest_type = type, .io_semantics = io);
      b->shader->info.per_primitive_inputs |= BITFIELD64_BIT(slot);
   } else if (s->interpolation[slot] == INTERP_MODE_FLAT) {
      value = nir_load_input(b, components, bits, nir_imm_int(b, 0),
         .dest_type = type, .io_semantics = io);
   } else {
      enum glsl_interp_mode mode = s->interpolation[slot];
      nir_def *bary;
      if (kind == 0)
         bary = nir_load_barycentric_at_sample(b, 32, nir_imm_int(b, 0),
                                                .interp_mode = mode);
      else if (kind == 1)
         bary = nir_load_barycentric_centroid(b, 32, .interp_mode = mode);
      else
         bary = nir_load_barycentric_at_sample(b, 32, sample, .interp_mode = mode);
      value = nir_load_interpolated_input(b, components, bits, bary,
         nir_imm_int(b, 0), .dest_type = type, .io_semantics = io);
   }
   if (bits == 16) {
      nir_alu_type base = nir_alu_type_get_base_type(type);
      value = base == nir_type_float ? nir_f2f32(b, value) :
              base == nir_type_int ? nir_i2i32(b, value) : nir_u2u32(b, value);
   }
   return value;
}

/* The caller supplies the address of this native quad/sample's compact input
 * record. Each interpolation kind has a four-lane plane. */
static void
kk_interlock_capture_inputs_if(nir_builder *b,
                                 const struct kk_interlock_capture_nir *s,
                                 nir_def *record, nir_def *sample,
                                 nir_def *store, nir_def *coord)
{
   const uint64_t masks[] = {s->center, s->centroid, s->sample};
   nir_def *values[3][NUM_TOTAL_VARYING_SLOTS][4];
   nir_def *coords[4], *points[4];
   if (s->point_coord) {
      nir_def *point = nir_load_point_coord(b);
      nir_def *target = s->point_sample ? nir_load_sample_pos_from_id(b, 32, sample) :
         nir_imm_vec2(b, 0.5f, 0.5f);
      nir_def *offset = nir_fsub(b, target, nir_load_sample_pos(b));
      point = nir_ffma(b, nir_ddy(b, point), nir_channel(b, offset, 1),
         nir_ffma(b, nir_ddx(b, point), nir_channel(b, offset, 0), point));
      for (unsigned q = 0; q < 4; ++q)
         points[q] = nir_quad_broadcast(b, point, nir_imm_int(b, q));
   }
   for (unsigned kind = 0; kind < ARRAY_SIZE(masks); ++kind) {
      u_foreach_bit64(slot, masks[kind]) {
         nir_def *value = kk_interlock_capture_input(b, s, slot, kind, sample);
         for (unsigned q = 0; q < 4; ++q)
            values[kind][slot][q] = nir_quad_broadcast(b, value,
                                                       nir_imm_int(b, q));
      }
   }
   if (coord)
      for (unsigned q = 0; q < 4; ++q)
         coords[q] = nir_quad_broadcast(b, coord, nir_imm_int(b, q));
   nir_push_if(b, nir_iand(b, store,
                           nir_inot(b, nir_is_helper_invocation(b, 1))));
   for (unsigned kind = 0; kind < ARRAY_SIZE(masks); ++kind) {
      u_foreach_bit64(slot, masks[kind]) {
         for (unsigned q = 0; q < 4; ++q) {
            unsigned offset = s->kind_offset[kind] +
               s->layout[kind].slot_offset[slot] + q * s->layout[kind].stride;
            nir_store_global(b, values[kind][slot][q],
               nir_iadd_imm(b, record, offset), .align_mul = 4);
         }
      }
   }
   if (s->point_coord)
      for (unsigned q = 0; q < 4; ++q)
         nir_store_global(b, points[q], nir_iadd_imm(b, record, s->stride + 32 + q * 8),
            .align_mul = 4);
   if (coord)
      for (unsigned q = 0; q < 4; ++q)
         nir_store_global(b, coords[q],
            nir_iadd_imm(b, record, s->stride + q * coord->num_components * 4),
            .align_mul = 4);
   nir_pop_if(b, NULL);
}

static void
kk_interlock_capture_inputs(nir_builder *b,
                              const struct kk_interlock_capture_nir *s,
                              nir_def *record, nir_def *sample)
{
   kk_interlock_capture_inputs_if(b, s, record, sample, nir_imm_true(b), NULL);
}

/* A reservation chooses one input-record producer; losing physical samples keep
 * their geometric coverage and perform no application work. All lanes in a
 * winning quad still evaluate interpolants before conditional record stores. */
static nir_def *
kk_interlock_capture_claim_if(nir_builder *b, nir_def *address,
                                nir_def *eligible)
{
   nir_variable *won = nir_local_variable_create(b->impl, glsl_bool_type(), NULL);
   nir_store_var(b, won, nir_imm_false(b), 1);
   nir_push_if(b, nir_iand(b, eligible,
      nir_inot(b, nir_is_helper_invocation(b, 1))));
   nir_def *ticket = nir_global_atomic(b, 32, address, nir_imm_int(b, 1),
                                       .atomic_op = nir_atomic_op_iadd);
   nir_store_var(b, won, nir_ieq_imm(b, ticket, 0), 1);
   nir_pop_if(b, NULL);
   return nir_load_var(b, won);
}

static nir_def *
kk_interlock_capture_claim(nir_builder *b, nir_def *address)
{
   return kk_interlock_capture_claim_if(b, address, nir_imm_true(b));
}

/* Construct separate interpolation blocks with constant sample indices.
 * Covered and helper lanes therefore use the same interpolation index. */
static void
kk_interlock_capture_samples(nir_builder *b,
                               const struct kk_interlock_capture_nir *capture,
                               unsigned max_samples, nir_def *sample_count,
                               nir_def *won, nir_def *coord, void *data,
                               nir_def *(*record)(nir_builder *, void *, unsigned))
{
   nir_push_if(b, nir_quad_vote_any(b, 1, won));
   for (unsigned i = 0; i < max_samples; ++i) {
      nir_def *sample = nir_imm_int(b, i);
      if (sample_count)
         nir_push_if(b, nir_ult(b, sample, sample_count));
      kk_interlock_capture_inputs_if(b, capture, record(b, data, i), sample,
                                       won, coord);
      if (sample_count)
         nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
}

/* record() addresses one original primitive, quad and requested sample. Its
 * implementation reads shader temporaries initialized by the transaction
 * entry, so no SSA value crosses the temporary original function boundary. */
struct kk_interlock_captured_inputs_nir {
   const struct kk_interlock_capture_nir *capture;
   bool sample_shading;
   nir_variable *sample;
   nir_variable *coord;
   void *data;
   nir_def *(*record)(nir_builder *, void *, nir_def *sample);
   nir_def *(*sample_position)(nir_builder *, void *, nir_def *sample);
};

/* Center planes use native sample zero, whose helper values remain defined.
 * Interpolation-at-offset is valid in divergent control flow. Derive the
 * affine numerator and reciprocal-W planes from the captured quad with direct
 * loads, rather than executing derivatives or quad operations in that flow. */
static nir_def *
kk_interlock_captured_offset(nir_builder *b, nir_def *record,
   nir_def *address, const struct kk_interlock_capture_nir *capture,
   unsigned lane_stride, unsigned components, nir_def *pixel,
   nir_def *offset, nir_def *base_offset, bool perspective)
{
   nir_def *values[4];
   nir_def *weights[4];
   for (unsigned lane = 0; lane < 4; ++lane) {
      values[lane] = nir_load_global_constant(b, components, 32,
         nir_iadd_imm(b, address, lane * lane_stride), .align_mul = 4);
      weights[lane] = perspective ? nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, record, capture->stride + lane * 8 + 4),
         .align_mul = 4) : nir_imm_float(b, 1.0f);
   }
   if (perspective) {
      nir_def *center_weights[4];
      memcpy(center_weights, weights, sizeof(weights));
      for (unsigned lane = 0; lane < 4; ++lane) {
         unsigned row = lane & 2u, column = lane & 1u;
         nir_def *wx = nir_fsub(b, center_weights[row + 1], center_weights[row]);
         nir_def *wy = nir_fsub(b, center_weights[column + 2], center_weights[column]);
         weights[lane] = nir_ffma(b, wy, nir_channel(b, base_offset, 1),
            nir_ffma(b, wx, nir_channel(b, base_offset, 0), center_weights[lane]));
         values[lane] = nir_fmul(b, values[lane], weights[lane]);
      }
   }
   offset = nir_fsub(b, offset, base_offset);
   nir_def *right = nir_ine_imm(b, nir_iand_imm(b, nir_channel(b, pixel, 0), 1), 0);
   nir_def *bottom = nir_ine_imm(b, nir_iand_imm(b, nir_channel(b, pixel, 1), 1), 0);
   nir_def *value = nir_bcsel(b, bottom,
      nir_bcsel(b, right, values[3], values[2]),
      nir_bcsel(b, right, values[1], values[0]));
   nir_def *dx = nir_bcsel(b, bottom, nir_fsub(b, values[3], values[2]),
                                             nir_fsub(b, values[1], values[0]));
   nir_def *dy = nir_bcsel(b, right, nir_fsub(b, values[3], values[1]),
                                            nir_fsub(b, values[2], values[0]));
   nir_def *x = nir_channel(b, offset, 0), *y = nir_channel(b, offset, 1);
   value = nir_ffma(b, dy, y, nir_ffma(b, dx, x, value));
   if (perspective) {
      nir_def *weight = nir_bcsel(b, bottom,
         nir_bcsel(b, right, weights[3], weights[2]),
         nir_bcsel(b, right, weights[1], weights[0]));
      nir_def *wx = nir_bcsel(b, bottom,
         nir_fsub(b, weights[3], weights[2]), nir_fsub(b, weights[1], weights[0]));
      nir_def *wy = nir_bcsel(b, right,
         nir_fsub(b, weights[3], weights[1]), nir_fsub(b, weights[2], weights[0]));
      weight = nir_ffma(b, wy, y, nir_ffma(b, wx, x, weight));
      value = nir_fdiv(b, value, weight);
   }
   return value;
}

static bool
kk_interlock_captured_input(nir_builder *b, nir_intrinsic_instr *intr,
                              void *data)
{
   struct kk_interlock_captured_inputs_nir *s = data;
   if (intr->intrinsic == nir_intrinsic_load_point_coord && s->capture->point_coord) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_def *sample = s->sample_shading ? nir_load_var(b, s->sample) : nir_imm_int(b, 0);
      nir_def *record = s->record(b, s->data, sample);
      nir_def *pixel = nir_f2u32(b, nir_channels(b, nir_load_var(b, s->coord), 3));
      nir_def_replace(&intr->def, kk_interlock_quad_load(b,
         nir_iadd_imm(b, record, s->capture->stride + 32), 8, 2, 32, pixel));
      return true;
   }
   bool interpolated = intr->intrinsic == nir_intrinsic_load_interpolated_input;
   if (!interpolated && intr->intrinsic != nir_intrinsic_load_input &&
       intr->intrinsic != nir_intrinsic_load_per_primitive_input)
      return false;
   nir_src offset = intr->src[interpolated ? 1 : 0];
   assert(nir_src_is_const(offset));
   unsigned slot = nir_intrinsic_io_semantics(intr).location +
                   nir_src_as_uint(offset);
   unsigned kind = 0;
   nir_intrinsic_instr *bary = interpolated ?
      nir_src_as_intrinsic(intr->src[0]) : NULL;
   bool at_offset = bary &&
      bary->intrinsic == nir_intrinsic_load_barycentric_at_offset;
   if (bary) {
      switch (bary->intrinsic) {
      case nir_intrinsic_load_barycentric_pixel:
      case nir_intrinsic_load_barycentric_at_offset: kind = 0; break;
      case nir_intrinsic_load_barycentric_centroid: kind = 1; break;
      case nir_intrinsic_load_barycentric_sample:
      case nir_intrinsic_load_barycentric_at_sample: kind = 2; break;
      default: UNREACHABLE("captured interlock interpolation");
      }
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *sample = bary &&
      bary->intrinsic == nir_intrinsic_load_barycentric_at_sample ?
      bary->src[0].ssa : nir_load_var(b, s->sample);
   /* Pixel-frequency center/centroid/flat inputs use one coherent captured
    * quad. Explicit interpolateAtSample retains its requested sample. */
   if (!s->sample_shading && kind != 2)
      sample = nir_imm_int(b, 0);
   nir_def *record = s->record(b, s->data, sample);
   const struct kk_interlock_quad_layout *layout = &s->capture->layout[kind];
   nir_def *address = nir_iadd_imm(b, record,
      s->capture->kind_offset[kind] + layout->slot_offset[slot] +
      nir_intrinsic_component(intr) *
         (MAX2(nir_alu_type_get_type_size(s->capture->types[slot]), 32) / 8));
   nir_def *pixel = nir_f2u32(b, nir_channels(b,
      nir_load_var(b, s->coord), 3));
   unsigned bits = intr->def.bit_size;
   nir_def *value;
   if (bary && kind == 0) {
      assert(s->sample_position);
      nir_def *base_offset = nir_fadd_imm(b,
         s->sample_position(b, s->data, nir_imm_int(b, 0)), -0.5f);
      value = kk_interlock_captured_offset(b, record, address, s->capture,
         layout->stride, intr->num_components, pixel,
         at_offset ? bary->src[0].ssa : nir_imm_vec2(b, 0, 0), base_offset,
         nir_intrinsic_interp_mode(bary) == INTERP_MODE_SMOOTH);
   } else {
      value = kk_interlock_quad_load(b, address, layout->stride,
         intr->num_components, MAX2(bits, 32), pixel);
   }
   if (bits == 16)
      value = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) ==
         nir_type_float ? nir_f2f16(b, value) : nir_u2u16(b, value);
   nir_def_replace(&intr->def, value);
   return true;
}

static bool
kk_interlock_lower_captured_inputs(nir_shader *shader,
                                     struct kk_interlock_captured_inputs_nir *s)
{
   return nir_shader_intrinsics_pass(shader, kk_interlock_captured_input,
                                      nir_metadata_control_flow, s);
}

#endif
