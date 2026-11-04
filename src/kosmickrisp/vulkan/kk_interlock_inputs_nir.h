/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_INPUTS_NIR_H
#define KK_INTERLOCK_INPUTS_NIR_H

#include "compiler/nir/nir_builder.h"

/* Coordinates are in framebuffer space after the captured viewport transform.
 * Computing the same plane for uncovered lanes preserves derivative helpers;
 * coverage suppresses side effects, not interpolation or quad operations. */
static nir_def *
kk_interlock_weights_nir(nir_builder *b, nir_def *point,
                         nir_def *a, nir_def *c, nir_def *d)
{
   nir_def *u = nir_fsub(b, nir_channels(b, c, 3), nir_channels(b, a, 3));
   nir_def *v = nir_fsub(b, nir_channels(b, d, 3), nir_channels(b, a, 3));
   nir_def *p = nir_fsub(b, point, nir_channels(b, a, 3));
   nir_def *det = nir_fsub(b,
      nir_fmul(b, nir_channel(b, u, 0), nir_channel(b, v, 1)),
      nir_fmul(b, nir_channel(b, u, 1), nir_channel(b, v, 0)));
   nir_def *uc = nir_fdiv(b, nir_fsub(b,
      nir_fmul(b, nir_channel(b, p, 0), nir_channel(b, v, 1)),
      nir_fmul(b, nir_channel(b, p, 1), nir_channel(b, v, 0))), det);
   nir_def *vc = nir_fdiv(b, nir_fsub(b,
      nir_fmul(b, nir_channel(b, u, 0), nir_channel(b, p, 1)),
      nir_fmul(b, nir_channel(b, u, 1), nir_channel(b, p, 0))), det);
   return nir_vec3(b,
      nir_fsub(b, nir_imm_float(b, 1), nir_fadd(b, uc, vc)), uc, vc);
}

static nir_def *
kk_interlock_interpolate_nir(nir_builder *b, nir_def *weights,
                              nir_def *reciprocal_w,
                              nir_def *a, nir_def *c, nir_def *d,
                              bool perspective)
{
   if (perspective) {
      weights = nir_fmul(b, weights, reciprocal_w);
      nir_def *sum = nir_fadd(b, nir_channel(b, weights, 0),
         nir_fadd(b, nir_channel(b, weights, 1), nir_channel(b, weights, 2)));
      weights = nir_fdiv(b, weights, sum);
   }
   return nir_fadd(b,
      nir_fmul(b, a, nir_channel(b, weights, 0)),
      nir_fadd(b, nir_fmul(b, c, nir_channel(b, weights, 1)),
                   nir_fmul(b, d, nir_channel(b, weights, 2))));
}

static nir_def *
kk_interlock_sample_point_nir(nir_builder *b, nir_def *pixel,
                               nir_def *sample_position)
{
   return nir_fadd(b, nir_ffloor(b, pixel), sample_position);
}

struct kk_interlock_inputs_nir {
   nir_def *raster_primitive;
   nir_def *position[3];
   nir_def *reciprocal_w;
   nir_def *centroid_point;
   nir_def *sample_positions;
   nir_def *provoking_vertex;
   nir_def *front_facing;
   void *data;
   nir_def *(*load_vertex)(nir_builder *,
      const struct kk_interlock_inputs_nir *, nir_def *vertex,
      unsigned location, unsigned component, unsigned components,
      unsigned bits, nir_alu_type type);
};

static nir_def *
kk_interlock_sample_position_nir(nir_builder *b,
                                  const struct kk_interlock_inputs_nir *s,
                                  nir_def *sample)
{
   /* Out-of-range interpolateAtSample arguments have undefined values. Keep
    * their loads inside the private table nevertheless. */
   nir_def *offset = nir_u2u64(b,
      nir_imul_imm(b, nir_iand_imm(b, sample, 31), 8));
   return nir_load_global_constant(b, 2, 32,
      nir_iadd(b, s->sample_positions, offset), .align_mul = 4);
}

static nir_def *
kk_interlock_interpolation_point_nir(nir_builder *b,
                                     const struct kk_interlock_inputs_nir *s,
                                     nir_intrinsic_instr *barycentric)
{
   nir_def *pixel = nir_channels(b, nir_load_frag_coord(b), 3);
   nir_def *center = nir_fadd_imm(b, nir_ffloor(b, pixel), 0.5);
   switch (barycentric->intrinsic) {
   case nir_intrinsic_load_barycentric_pixel:
      return center;
   case nir_intrinsic_load_barycentric_centroid:
      return s->centroid_point;
   case nir_intrinsic_load_barycentric_sample:
      return kk_interlock_sample_point_nir(b, pixel,
         kk_interlock_sample_position_nir(b, s, nir_load_sample_id(b)));
   case nir_intrinsic_load_barycentric_at_sample:
      return kk_interlock_sample_point_nir(b, pixel,
         kk_interlock_sample_position_nir(b, s,
                                           barycentric->src[0].ssa));
   case nir_intrinsic_load_barycentric_at_offset:
      return nir_fadd(b, center, barycentric->src[0].ssa);
   default:
      UNREACHABLE("interlock interpolation mode");
   }
}

static bool
kk_interlock_input_nir(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct kk_interlock_inputs_nir *s = data;
   if (&intr->def == s->raster_primitive)
      return false;
   nir_def *value;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_sample_pos_from_id:
      b->cursor = nir_before_instr(&intr->instr);
      value = kk_interlock_sample_position_nir(b, s, intr->src[0].ssa);
      break;
   case nir_intrinsic_load_front_face:
      b->cursor = nir_before_instr(&intr->instr);
      value = s->front_facing;
      break;
   case nir_intrinsic_load_primitive_id:
      b->cursor = nir_before_instr(&intr->instr);
      value = s->load_vertex(b, s, s->provoking_vertex,
         VARYING_SLOT_PRIMITIVE_ID, 0, 1, 32, nir_type_uint32);
      break;
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input: {
      bool interpolated = intr->intrinsic ==
                           nir_intrinsic_load_interpolated_input;
      nir_src offset = intr->src[interpolated ? 1 : 0];
      assert(nir_src_is_const(offset));
      unsigned location = nir_intrinsic_io_semantics(intr).location +
                           nir_src_as_uint(offset);
      unsigned component = nir_intrinsic_component(intr);
      unsigned components = intr->num_components;
      unsigned bits = intr->def.bit_size;
      nir_alu_type type = nir_intrinsic_dest_type(intr);
      b->cursor = nir_before_instr(&intr->instr);
      if (!interpolated) {
         value = s->load_vertex(b, s, s->provoking_vertex, location,
                                component, components, bits, type);
         break;
      }
      nir_intrinsic_instr *barycentric = nir_src_as_intrinsic(intr->src[0]);
      nir_def *point = kk_interlock_interpolation_point_nir(b, s, barycentric);
      nir_def *weights = kk_interlock_weights_nir(b, point,
         s->position[0], s->position[1], s->position[2]);
      nir_def *vertices[3];
      for (unsigned i = 0; i < ARRAY_SIZE(vertices); ++i)
         vertices[i] = s->load_vertex(b, s, nir_imm_int(b, i), location,
                                      component, components, bits, type);
      bool perspective = nir_intrinsic_interp_mode(barycentric) !=
                            INTERP_MODE_NOPERSPECTIVE;
      /* Interpolation operates at 32 bits even for packed half inputs. */
      if (bits == 16)
         for (unsigned i = 0; i < ARRAY_SIZE(vertices); ++i)
            vertices[i] = nir_f2f32(b, vertices[i]);
      value = kk_interlock_interpolate_nir(b, weights, s->reciprocal_w,
         vertices[0], vertices[1], vertices[2], perspective);
      if (bits == 16)
         value = nir_f2f16(b, value);
      break;
   }
   default:
      return false;
   }
   nir_def_replace(&intr->def, value);
   return true;
}

static bool
kk_interlock_lower_inputs_nir(nir_shader *shader,
                               struct kk_interlock_inputs_nir *state)
{
   return nir_shader_intrinsics_pass(shader, kk_interlock_input_nir,
                                      nir_metadata_control_flow, state);
}

#endif
