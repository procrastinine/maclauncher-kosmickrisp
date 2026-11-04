/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT */
#include "kk_conservative.h"
#include "kk_nir_vertex_triplet.h"
#include "kk_conservative_fs_link.h"

static bool
kk_conservative_possible(const struct vk_graphics_pipeline_state *state)
{
   return state && state->rs &&
      (state->rs->conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_CONSERVATIVE_MODE));
}

static nir_def *
kk_conservative_load(nir_builder *b, nir_def *packet, unsigned n, unsigned bits,
                      unsigned offset)
{
   return nir_load_global_constant(b, n, bits, nir_iadd_imm(b, packet, offset),
                                    .align_mul = bits / 8);
}
#define cr_load(b, p, n, bits, member) \
   kk_conservative_load(b, p, n, bits, offsetof(struct kk_conservative_primitive, member))

static nir_def *
kk_conservative_packet(nir_builder *b)
{
   return nir_pack_64_2x32(b, nir_load_input(b, 2, 32, nir_imm_int(b, 0),
      .dest_type = nir_type_uint32,
      .io_semantics.location = KK_CONSERVATIVE_ADDRESS_SLOT,
      .io_semantics.num_slots = 1));
}

#include "kk_conservative_inner_nir.h"

/* The input mask belongs to the original conservative primitive. A row
 * triangle's native mask describes only part of that primitive's samples. */
static nir_def *
kk_conservative_sample_mask(nir_builder *b, nir_def *packet, bool sample_shading)
{
   nir_def *samples = cr_load(b, packet, 1, 32, samples);
   nir_def *mask = nir_ushr(b, nir_imm_int(b, -1),
      nir_isub(b, nir_imm_int(b, 32), samples));
   mask = nir_iand(b, mask, cr_load(b, packet, 1, 32, sample_mask));
   if (sample_shading)
      mask = nir_iand(b, mask,
         nir_ishl(b, nir_imm_int(b, 1), nir_load_sample_id(b)));
   return mask;
}

/* Homogeneous barycentrics avoid divisions by the original vertices' W.
 * Clipped primitives may have an original corner on or behind the eye plane. */
static nir_def *
kk_conservative_weights(nir_builder *b, nir_def *packet, nir_def *xy,
                         bool linear, nir_def **reciprocal_w, nir_def **depth)
{
   nir_def *p[3], *x[3], *y[3], *v[3];
   for (unsigned i = 0; i < 3; ++i) {
      p[i] = kk_conservative_load(b, packet, 4, 32,
         offsetof(struct kk_conservative_primitive, positions) + i * 16);
      x[i] = nir_fsub(b, nir_channel(b, p[i], 0),
                      nir_fmul(b, nir_channel(b, xy, 0), nir_channel(b, p[i], 3)));
      y[i] = nir_fsub(b, nir_channel(b, p[i], 1),
                      nir_fmul(b, nir_channel(b, xy, 1), nir_channel(b, p[i], 3)));
   }
   for (unsigned i = 0; i < 3; ++i) {
      unsigned j = (i + 1) % 3, k = (i + 2) % 3;
      v[i] = nir_fsub(b, nir_fmul(b, x[j], y[k]), nir_fmul(b, x[k], y[j]));
   }
   nir_def *degenerate = nir_ine_imm(b, cr_load(b, packet, 1, 32, degenerate), 0);
   nir_def *provoking = cr_load(b, packet, 1, 32, provoking);
   nir_def *prov = nir_bcsel(b, nir_ieq_imm(b, provoking, 0), p[0],
      nir_bcsel(b, nir_ieq_imm(b, provoking, 1), p[1], p[2]));
   nir_def *sum = nir_fadd(b, nir_fadd(b, v[0], v[1]), v[2]);
   nir_def *den = nir_imm_float(b, 0), *z = nir_imm_float(b, 0);
   for (unsigned i = 0; i < 3; ++i) {
      den = nir_fadd(b, den, nir_fmul(b, v[i], nir_channel(b, p[i], 3)));
      z = nir_fadd(b, z, nir_fmul(b, v[i], nir_channel(b, p[i], 2)));
   }
   if (reciprocal_w)
      *reciprocal_w = nir_bcsel(b, degenerate, nir_frcp(b, nir_channel(b, prov, 3)), nir_fdiv(b, sum, den));
   if (depth)
      *depth = nir_fadd(b, nir_bcsel(b, degenerate, nir_fdiv(b, nir_channel(b, prov, 2), nir_channel(b, prov, 3)), nir_fdiv(b, z, den)), cr_load(b, packet, 1, 32, bias));
   for (unsigned i = 0; i < 3; ++i) {
      if (linear)
         v[i] = nir_fmul(b, v[i], nir_channel(b, p[i], 3));
      v[i] = nir_fdiv(b, v[i], linear ? den : sum);
   }
   for (unsigned i = 0; i < 3; ++i)
      v[i] = nir_bcsel(b, degenerate,
         nir_b2f32(b, nir_ieq_imm(b, provoking, i)), v[i]);
   return nir_vec3(b, v[0], v[1], v[2]);
}

static nir_def *
kk_conservative_eval_position(nir_builder *b, nir_intrinsic_instr *bary)
{
   nir_def *pixel = nir_fadd_imm(b, nir_ffloor(b,
      nir_channels(b, nir_load_frag_coord(b), 3)), 0.5f);
   switch (bary->intrinsic) {
   case nir_intrinsic_load_barycentric_at_offset:
   case nir_intrinsic_load_barycentric_coord_at_offset:
      return nir_fadd(b, pixel, bary->src[0].ssa);
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_coord_at_sample:
      return nir_fadd(b, nir_fadd_imm(b, pixel, -0.5f),
         nir_load_sample_pos_from_id(b, 32, bary->src[0].ssa));
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_coord_sample:
      return nir_fadd(b, nir_fadd_imm(b, pixel, -0.5f), nir_load_sample_pos(b));
   default:
      /* Every sample of an accepted conservative pixel is covered, so the
       * centroid is its center. */
      return pixel;
   }
}

struct kk_conservative_fs_context {
   const struct poly_gs_capture_info *info;
   const struct kk_per_vertex_map *canonical;
   bool sample_shading;
};

static bool
kk_conservative_mask_output(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if ((intr->intrinsic != nir_intrinsic_store_output &&
        intr->intrinsic != nir_intrinsic_load_output) ||
       nir_intrinsic_io_semantics(intr).location != FRAG_RESULT_SAMPLE_MASK)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_variable *mask = data;
   if (intr->intrinsic == nir_intrinsic_store_output) {
      nir_store_var(b, mask, intr->src[0].ssa, 1);
      nir_instr_remove(&intr->instr);
   } else {
      nir_def_replace(&intr->def, nir_load_var(b, mask));
   }
   return true;
}

static bool
kk_conservative_fs_intrinsic(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct kk_conservative_fs_context *context = data;
   const struct poly_gs_capture_info *info = context->info;
   bool interpolated = intr->intrinsic == nir_intrinsic_load_interpolated_input;
   bool flat = intr->intrinsic == nir_intrinsic_load_input;
   bool per_vertex = intr->intrinsic == nir_intrinsic_load_per_vertex_input ||
                     intr->intrinsic == nir_intrinsic_load_input_vertex;
   bool barycentric =
      intr->intrinsic >= nir_intrinsic_load_barycentric_coord_at_offset &&
      intr->intrinsic <= nir_intrinsic_load_barycentric_coord_sample;
   bool position = intr->intrinsic == nir_intrinsic_load_frag_coord;
   bool front = intr->intrinsic == nir_intrinsic_load_front_face;
   bool mask = intr->intrinsic == nir_intrinsic_load_sample_mask_in;
   bool fully = intr->intrinsic == nir_intrinsic_load_fully_covered;
   bool fsr_primitive = intr->intrinsic == nir_intrinsic_load_raster_primitive_id_kk;
   if (!interpolated && !flat && !per_vertex && !barycentric && !position && !front && !mask && !fully && !fsr_primitive)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *packet = kk_conservative_packet(b), *value;
   if (fsr_primitive) {
      /* All row triangles of one original primitive share its packet. It is
       * 8-byte aligned in one 128 MiB heap, so its address in 8-byte units is
       * injective modulo 2^32 within that heap, including an address wrap.
       * Physical row triangle IDs can differ between a pixel's samples. */
      value = nir_u2u32(b, nir_ushr_imm(b, packet, 3));
   } else if (fully) {
      nir_def *xy = nir_f2i32(b, nir_ffloor(b, nir_channels(b, nir_load_frag_coord(b), 3)));
      nir_def *rectangle = nir_vec4(b, nir_channel(b, xy, 0), nir_channel(b, xy, 1),
         nir_iadd_imm(b, nir_channel(b, xy, 0), 1),
         nir_iadd_imm(b, nir_channel(b, xy, 1), 1));
      value = kk_conservative_evaluate_inner_coverage(b, packet, rectangle);
   } else if (mask) {
      value = kk_conservative_sample_mask(b, packet, context->sample_shading);
   } else if (position) {
      nir_def *raw = nir_load_frag_coord(b), *rho, *depth;
      nir_def *xy = nir_channels(b, raw, 3);
      if (context->sample_shading)
         xy = nir_fadd(b, xy, nir_fadd_imm(b,
            nir_load_sample_pos_from_id(b, 32, nir_load_sample_id(b)), -0.5f));
      kk_conservative_weights(b, packet, xy, false, &rho, &depth);
      nir_def *range = kk_conservative_load(b, packet, 2, 32,
         offsetof(struct kk_conservative_primitive, viewport) + 16);
      nir_def *minimum = nir_fmin(b, nir_channel(b, range, 0), nir_channel(b, range, 1));
      nir_def *maximum = nir_fmax(b, nir_channel(b, range, 0), nir_channel(b, range, 1));
      depth = nir_bcsel(b, nir_ine_imm(b, cr_load(b, packet, 1, 32, depth_clamp), 0),
         nir_fclamp(b, depth, minimum, maximum), depth);
      value = nir_vec4(b, nir_channel(b, raw, 0), nir_channel(b, raw, 1), depth, rho);
   } else if (front) {
      value = nir_ine_imm(b, cr_load(b, packet, 1, 32, front), 0);
   } else if (barycentric) {
      value = kk_conservative_weights(b, packet,
         kk_conservative_eval_position(b, intr),
         nir_intrinsic_interp_mode(intr) == INTERP_MODE_NOPERSPECTIVE, NULL, NULL);
   } else {
      unsigned offset_src = flat ? 0 : 1;
      unsigned location = nir_intrinsic_io_semantics(intr).location +
                          nir_src_as_uint(intr->src[offset_src]);
      if (context->canonical && flat && location == context->canonical->buffer_slot) {
         value = nir_unpack_64_2x32(b, cr_load(b, packet, 1, 64, canonical));
         nir_def_replace(&intr->def, nir_channels(b, value,
            BITFIELD_MASK(intr->def.num_components) << nir_intrinsic_component(intr)));
         return true;
      }
      if (context->canonical && interpolated &&
          (location == context->canonical->barycentric_slot[0] ||
           location == context->canonical->barycentric_slot[1])) {
         nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
         value = kk_conservative_weights(b, packet,
            kk_conservative_eval_position(b, bary),
            location == context->canonical->barycentric_slot[1], NULL, NULL);
         nir_def_replace(&intr->def, nir_channels(b, value,
            BITFIELD_MASK(intr->def.num_components) << nir_intrinsic_component(intr)));
         return true;
      }
      unsigned packed = util_bitcount64(info->outputs & BITFIELD64_MASK(location));
      nir_def *values[3];
      for (unsigned v = 0; v < 3; ++v) {
         nir_def *address = cr_load(b, packet, 1, 64, corners);
         if (v)
            address = kk_conservative_load(b, packet, 1, 64,
               offsetof(struct kk_conservative_primitive, corners) + v * 8);
         nir_def *raw = kk_nir_load_primitive_corner(b, address,
            packed * 16 + nir_intrinsic_component(intr) * 4,
            intr->def.num_components, 32);
         bool floating = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) ==
                         nir_type_float;
         values[v] = intr->def.bit_size == 32 ? raw :
            floating ? nir_f2fN(b, raw, intr->def.bit_size) :
                       nir_u2uN(b, raw, intr->def.bit_size);
      }
      if (flat || per_vertex) {
         nir_def *vertex = flat ? cr_load(b, packet, 1, 32, provoking) : intr->src[0].ssa;
         value = nir_bcsel(b, nir_ieq_imm(b, vertex, 0), values[0],
            nir_bcsel(b, nir_ieq_imm(b, vertex, 1), values[1], values[2]));
      } else {
         nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
         nir_def *weights = kk_conservative_weights(b, packet,
            kk_conservative_eval_position(b, bary),
            nir_intrinsic_interp_mode(bary) == INTERP_MODE_NOPERSPECTIVE, NULL, NULL);
         value = nir_imm_floatN_t(b, 0.0f, intr->def.bit_size);
         for (unsigned v = 0; v < 3; ++v)
            value = nir_fadd(b, value, nir_fmul(b, values[v],
               nir_f2fN(b, nir_channel(b, weights, v), intr->def.bit_size)));
         nir_def *provoking = cr_load(b, packet, 1, 32, provoking);
         nir_def *prov = nir_bcsel(b, nir_ieq_imm(b, provoking, 0), values[0],
            nir_bcsel(b, nir_ieq_imm(b, provoking, 1), values[1], values[2]));
         value = nir_bcsel(b, nir_ine_imm(b, cr_load(b, packet, 1, 32, degenerate), 0), prov, value);
      }
   }
   nir_def_replace(&intr->def, value);
   return true;
}

static void
kk_conservative_lower_fs_impl(nir_shader *fs, const struct poly_gs_capture_info *info,
                              const struct kk_per_vertex_map *canonical)
{
   bool sample_shading = fs->info.fs.uses_sample_shading;
   struct kk_conservative_fs_context context = {
      .info = info, .sample_shading = sample_shading, .canonical = canonical,
   };
   nir_function_impl *impl = nir_shader_get_entrypoint(fs);
   nir_variable *mask_output = nir_local_variable_create(impl, glsl_uint_type(),
                                                        "conservative_mask_out");
   nir_shader_intrinsics_pass(fs, kk_conservative_mask_output,
                              nir_metadata_control_flow, mask_output);
   nir_shader_intrinsics_pass(fs, kk_conservative_fs_intrinsic,
                              nir_metadata_control_flow, &context);
   nir_builder entry = nir_builder_at(nir_before_impl(impl));
   nir_store_var(&entry, mask_output, nir_imm_int(&entry, -1), 1);
   /* Metal early tests do not apply a pixel-rate sample-mask output to color
    * coverage. Execute its physical coverage test per sample, while keeping
    * application pixel-rate SampleMaskIn and interpolation unchanged. */
   nir_def *coverage = kk_conservative_sample_mask(&entry,
      kk_conservative_packet(&entry), false);
   if (sample_shading || fs->info.fs.early_fragment_tests)
      coverage = nir_iand(&entry, coverage,
         nir_ishl(&entry, nir_imm_int(&entry, 1), nir_load_sample_id(&entry)));
   nir_discard_if(&entry, nir_ieq_imm(&entry, coverage, 0));
   nir_builder finish = nir_builder_at(nir_after_impl(impl));
   nir_def *final_mask = nir_iand(&finish, nir_load_var(&finish, mask_output),
      kk_conservative_sample_mask(&finish, kk_conservative_packet(&finish), false));
   if (fs->info.fs.early_fragment_tests) {
      final_mask = nir_iand(&finish, final_mask,
         nir_ishl(&finish, nir_imm_int(&finish, 1), nir_load_sample_id(&finish)));
      nir_discard_if(&finish, nir_ieq_imm(&finish, final_mask, 0));
   }
   nir_store_output(&finish, final_mask, nir_imm_int(&finish, 0),
      .src_type = nir_type_uint32,
      .io_semantics.location = FRAG_RESULT_SAMPLE_MASK,
      .io_semantics.num_slots = 1);
   /* The compute expansion has already restricted rasterization to exactly
    * covered whole pixels, before early depth/stencil. A fragment reject is
    * unnecessary for early-test shaders, whose writes precede a discard. */
   if (!fs->info.fs.early_fragment_tests) {
      nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(fs)));
      nir_def *packet = kk_conservative_packet(&b);
      nir_def *xy = nir_fadd_imm(&b, nir_ffloor(&b,
         nir_channels(&b, nir_load_frag_coord(&b), 3)), 0.5f);
      nir_def *count = cr_load(&b, packet, 1, 32, count);
      nir_variable *index = nir_local_variable_create(b.impl, glsl_uint_type(), "edge");
      nir_store_var(&b, index, nir_imm_int(&b, 0), 1);
      nir_push_loop(&b);
      nir_def *i = nir_load_var(&b, index);
      nir_push_if(&b, nir_uge(&b, i, count));
      nir_jump(&b, nir_jump_break);
      nir_pop_if(&b, NULL);
      nir_def *edge = nir_load_global_constant(&b, 4, 32,
         nir_iadd(&b, packet, nir_u2u64(&b, nir_iadd_imm(&b,
            nir_imul_imm(&b, i, 16),
            offsetof(struct kk_conservative_primitive, edges)))), .align_mul = 4);
      nir_def *distance = nir_fadd(&b,
         nir_fdot2(&b, nir_channels(&b, edge, 3), xy),
         nir_fadd(&b, nir_channel(&b, edge, 2), nir_channel(&b, edge, 3)));
      nir_discard_if(&b, nir_flt_imm(&b, distance, 0));
      nir_store_var(&b, index, nir_iadd_imm(&b, i, 1), 1);
      nir_pop_loop(&b, NULL);
   }
   nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));
   fs->info.fs.uses_sample_shading = sample_shading;
}

static void
kk_conservative_lower_fs(nir_shader *fs, const struct poly_gs_capture_info *info)
{
   kk_conservative_lower_fs_impl(fs, info, NULL);
}

bool
kk_conservative_lower_canonical_fs(nir_shader *fs,
   const struct kk_per_vertex_map *canonical,
   const struct poly_gs_capture_info *producer)
{
   kk_conservative_lower_fs_impl(fs, producer, canonical);
   return true;
}

static nir_shader *
kk_conservative_vertex_shader(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                  "Poly conservative raster");
   b.shader->info.io_lowered = true;
   nir_def *id = nir_load_vertex_id(&b);
   nir_def *span = nir_udiv_imm(&b, id, 6), *corner = nir_umod_imm(&b, id, 6);
   nir_def *output = kk_polygon_load(&b, 1, 64,
      offsetof(struct poly_geometry_capture, output), 8);
   nir_def *span_address = nir_iadd(&b, output,
      nir_u2u64(&b, nir_imul_imm(&b, span, sizeof(struct kk_conservative_span))));
   nir_def *packet = nir_load_global_constant(&b, 1, 64, span_address, .align_mul = 8);
   nir_def *bounds = nir_load_global_constant(&b, 4, 32,
      nir_iadd_imm(&b, span_address, offsetof(struct kk_conservative_span, bounds)), .align_mul = 4);
   nir_def *front = cr_load(&b, packet, 1, 32, front);
   nir_def *front_ccw = cr_load(&b, packet, 1, 32, front_ccw);
   nir_def *reverse = nir_ieq(&b, front, front_ccw);
   nir_def *triangle = nir_udiv_imm(&b, corner, 3), *within = nir_umod_imm(&b, corner, 3);
   corner = nir_bcsel(&b, nir_iand(&b, reverse, nir_ine_imm(&b, within, 0)),
      nir_iadd(&b, nir_imul_imm(&b, triangle, 3), nir_isub(&b, nir_imm_int(&b, 3), within)), corner);
   const unsigned corners[6] = {0, 2, 3, 0, 3, 1};
   nir_def *which = nir_imm_int(&b, 0);
   for (unsigned i = 0; i < 6; ++i)
      which = nir_bcsel(&b, nir_ieq_imm(&b, corner, i), nir_imm_int(&b, corners[i]), which);
   nir_def *xy = nir_vec2(&b,
      nir_bcsel(&b, nir_ine_imm(&b, nir_iand_imm(&b, which, 2), 0),
         nir_channel(&b, bounds, 2), nir_channel(&b, bounds, 0)),
      nir_bcsel(&b, nir_ine_imm(&b, nir_iand_imm(&b, which, 1), 0),
         nir_channel(&b, bounds, 3), nir_channel(&b, bounds, 1)));
   nir_def *depth;
   kk_conservative_weights(&b, packet, xy, false, NULL, &depth);
   nir_def *range = kk_conservative_load(&b, packet, 2, 32,
      offsetof(struct kk_conservative_primitive, viewport) + 16);
   nir_def *minimum = nir_channel(&b, range, 0), *maximum = nir_channel(&b, range, 1);
   nir_def *mapped = nir_bcsel(&b, nir_feq(&b, minimum, maximum), nir_imm_float(&b, 0),
      nir_fdiv(&b, nir_fsub(&b, depth, minimum), nir_fsub(&b, maximum, minimum)));
   depth = nir_bcsel(&b, nir_ine_imm(&b, cr_load(&b, packet, 1, 32, depth_clamp), 0),
      mapped, depth);
   nir_def *size = cr_load(&b, packet, 2, 32, render_size);
   nir_def *ndc = nir_fadd_imm(&b, nir_fmul_imm(&b, nir_fdiv(&b, xy, size), 2), -1);
   nir_store_output(&b, nir_vec4(&b, nir_channel(&b, ndc, 0), nir_channel(&b, ndc, 1),
      depth, nir_imm_float(&b, 1)), nir_imm_int(&b, 0),
      .src_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_POS,
      .io_semantics.num_slots = 1);
   nir_store_output(&b, nir_unpack_64_2x32(&b, packet), nir_imm_int(&b, 0),
      .src_type = nir_type_uint32, .io_semantics.location = KK_CONSERVATIVE_ADDRESS_SLOT,
      .io_semantics.num_slots = 1);
   nir_store_output(&b, cr_load(&b, packet, 1, 32, layer), nir_imm_int(&b, 0),
      .src_type = nir_type_uint32, .io_semantics.location = VARYING_SLOT_LAYER,
      .io_semantics.num_slots = 1);
   nir_store_output(&b, cr_load(&b, packet, 1, 32, viewport_index), nir_imm_int(&b, 0),
      .src_type = nir_type_uint32, .io_semantics.location = VARYING_SLOT_VIEWPORT,
      .io_semantics.num_slots = 1);
   nir_shader_gather_info(b.shader, b.impl);
   return b.shader;
}
#undef cr_load
