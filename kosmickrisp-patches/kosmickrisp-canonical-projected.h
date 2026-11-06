/* SPDX-License-Identifier: MIT */
#ifndef KK_CANONICAL_PROJECTED_H
#define KK_CANONICAL_PROJECTED_H

static void
kk_canonical_store_position(nir_builder *b, nir_intrinsic_instr *intr,
                             unsigned location)
{
   unsigned start = nir_intrinsic_component(intr);
   unsigned mask = nir_intrinsic_write_mask(intr);
   for (unsigned c = 0; c < intr->num_components; ++c) {
      unsigned component = start + c;
      if (!(mask & BITFIELD_BIT(c)) || component == 2u)
         continue;
      nir_store_output(b, nir_channel(b, intr->src[0].ssa, c), nir_imm_int(b, 0),
         .component = component == 3u ? 2u : component, .write_mask = 1,
         .src_type = nir_type_float32, .io_semantics.location = location,
         .io_semantics.num_slots = 1);
   }
}

static void
kk_canonical_store_corner(nir_builder *b, nir_intrinsic_instr *intr,
                           unsigned corner, nir_def *buffer)
{
   unsigned start = nir_intrinsic_component(intr), mask = nir_intrinsic_write_mask(intr);
   nir_push_if(b, nir_ine_imm(b, buffer, 0));
   for (unsigned c = 0; c < intr->num_components; ++c) {
      unsigned component = start + c;
      if (!(mask & BITFIELD_BIT(c)) || component == 2u)
         continue;
      unsigned offset = kk_canonical_position_offset(corner) +
         (component == 3u ? 2u : component) * 4u;
      nir_build_store_global(b, nir_channel(b, intr->src[0].ssa, c),
         nir_iadd_imm(b, buffer, offset), .write_mask = 1, .align_mul = 4);
   }
   nir_pop_if(b, NULL);
}

/* An ordinary native interpolant supplies the requested evaluation point.
 * Unlike unit bases this carrier is identical at a shared vertex in every
 * primitive. Keep homogeneous coordinates until after rasterization so
 * vertices with zero or negative clip W do not require division in a producer.
 * Both VS and mesh producers use this canonical interface form. */
static nir_def *
kk_canonical_evaluation_point(nir_builder *b, nir_intrinsic_instr *original)
{
   nir_def *bary;
   switch (original->intrinsic) {
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_coord_centroid:
      bary = nir_load_barycentric_centroid(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
      break;
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_coord_sample:
      bary = nir_load_barycentric_at_sample(b, 32, nir_load_sample_id(b),
                                          .interp_mode = INTERP_MODE_SMOOTH);
      break;
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_coord_at_sample:
      bary = nir_load_barycentric_at_sample(b, 32, original->src[0].ssa,
                                          .interp_mode = INTERP_MODE_SMOOTH);
      break;
   case nir_intrinsic_load_barycentric_at_offset:
   case nir_intrinsic_load_barycentric_coord_at_offset:
      bary = nir_load_barycentric_at_offset(b, 32, original->src[0].ssa,
                                          .interp_mode = INTERP_MODE_SMOOTH);
      break;
   default:
      bary = nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
      break;
   }
   nir_def *p = nir_load_interpolated_input(
      b, 3, 32, bary, nir_imm_int(b, 0), .dest_type = nir_type_float32,
      .io_semantics.location = KK_CANONICAL_PROJECTED_POINT,
      .io_semantics.num_slots = 1);
   return nir_fdiv(b, nir_channels(b, p, 3), nir_channel(b, p, 2));
}

static nir_def *
kk_canonical_cross2(nir_builder *b, nir_def *a, nir_def *c)
{
   return nir_fsub(b, nir_fmul(b, nir_channel(b, a, 0), nir_channel(b, c, 1)),
                      nir_fmul(b, nir_channel(b, a, 1), nir_channel(b, c, 0)));
}

static nir_def *
kk_canonical_projected_weights(nir_builder *b, nir_intrinsic_instr *original)
{
   nir_def *p = kk_canonical_evaluation_point(b, original);
   nir_def *address = nir_pack_64_2x32(b, nir_load_input(
      b, 2, 32, nir_imm_int(b, 0), .dest_type = nir_type_uint32,
      .io_semantics.location = VARYING_SLOT_VAR0, .io_semantics.num_slots = 1));
   nir_def *corner[3], *ray[3];
   for (unsigned i = 0; i < 3; ++i) {
      corner[i] = kk_nir_load_primitive_corner(b, address,
         kk_canonical_position_offset(i), 3, 32);
      ray[i] = nir_fsub(b, nir_channels(b, corner[i], 3),
                          nir_fmul(b, p, nir_channel(b, corner[i], 2)));
   }
   nir_def *a = kk_canonical_cross2(b, ray[1], ray[2]);
   nir_def *c = kk_canonical_cross2(b, ray[2], ray[0]);
   nir_def *d = kk_canonical_cross2(b, ray[0], ray[1]);
   nir_def *sum = nir_fadd(b, nir_fadd(b, a, c), d);
   nir_def *safe_sum = nir_bcsel(b, nir_feq_imm(b, sum, 0), nir_imm_float(b, 1), sum);
   nir_def *triangle = nir_fdiv(b, nir_vec3(b, a, c, d), safe_sum);

   nir_def *edge = nir_fsub(b, ray[1], ray[0]);
   nir_def *denom = nir_fdot2(b, edge, edge);
   nir_def *safe_denom = nir_bcsel(b, nir_feq_imm(b, denom, 0), nir_imm_float(b, 1), denom);
   nir_def *t = nir_fdiv(b, nir_fneg(b, nir_fdot2(b, ray[0], edge)), safe_denom);
   nir_def *line = nir_vec3(b, nir_fsub(b, nir_imm_float(b, 1), t), t, nir_imm_float(b, 0));
   nir_def *topology = load_per_draw(b, 32, offsetof(struct kk_per_draw_data, per_vertex_topology));
   nir_def *point = nir_vec3(b, nir_imm_float(b, 1), nir_imm_float(b, 0), nir_imm_float(b, 0));
   nir_def *weights = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_POINTS), point,
      nir_bcsel(b, nir_ior(b, nir_ieq_imm(b, topology, MESA_PRIM_LINES),
                            nir_ieq_imm(b, topology, MESA_PRIM_LINE_STRIP)), line, triangle));
   if (nir_intrinsic_interp_mode(original) == INTERP_MODE_NOPERSPECTIVE) {
      nir_def *w = nir_vec3(b, nir_channel(b, corner[0], 2),
                              nir_channel(b, corner[1], 2),
                              nir_channel(b, corner[2], 2));
      weights = nir_fmul(b, weights, w);
      nir_def *total = nir_fadd(b, nir_fadd(b, nir_channel(b, weights, 0),
                                             nir_channel(b, weights, 1)),
                                  nir_channel(b, weights, 2));
      weights = nir_fdiv(b, weights, total);
   }
   return weights;
}

#endif
