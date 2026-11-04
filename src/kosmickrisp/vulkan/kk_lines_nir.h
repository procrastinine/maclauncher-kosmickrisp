/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT */

static bool
kk_line_effects_possible(const struct vk_graphics_pipeline_state *state)
{
   return kk_wide_line_possible(state) &&
      (state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
       state->rs->line.stipple.enable ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE) ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE));
}

/* Linked GS/tessellation output arity can rule out line state that the
 * pipeline-stage mask alone had to treat conservatively. */
static bool
kk_line_effects_for_primitive(const struct vk_graphics_pipeline_state *state,
                                unsigned primitive_vertices)
{
   return kk_line_effects_possible(state) &&
      (!primitive_vertices || primitive_vertices == 2 ||
       (primitive_vertices > 2 &&
        (state->rs->polygon_mode == VK_POLYGON_MODE_LINE ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE))));
}

/* Coverage is inserted before fixed-function blending. Native raster shaders
 * resolve the compile-time placeholder to one; expanded lines resolve it to
 * analytic rectangle coverage. No marker or extra work survives natively. */
static bool
kk_line_alpha(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output) return false;
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   if ((location != FRAG_RESULT_COLOR && location != FRAG_RESULT_DATA0) ||
       nir_alu_type_get_base_type(nir_intrinsic_src_type(intr)) != nir_type_float)
      return false;
   int alpha = 3 - (int)nir_intrinsic_component(intr);
   if (alpha < 0 || !(nir_intrinsic_write_mask(intr) & BITFIELD_BIT(alpha)))
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *value = intr->src[0].ssa;
   nir_def *coverage = nir_f2fN(b, nir_load_poly_line_smooth_coverage(b), value->bit_size);
   nir_src_rewrite(&intr->src[0], nir_vector_insert_imm(b, value,
      nir_fmul(b, nir_channel(b, value, alpha), coverage), alpha));
   return true;
}

static bool
kk_line_resolve_coverage(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_poly_line_smooth_coverage)
      return false;
   nir_def_replace(&intr->def, data);
   return true;
}

/* Stipple geometry preserves coverage at every MSAA sample without forcing
 * sample-frequency fragment shading. Observable early shaders use the same
 * path at one sample so OFF intervals never reach early depth/stencil tests. */
static bool
kk_line_geometry_stipple(const nir_shader *nir,
                          const struct vk_graphics_pipeline_state *state)
{
   /* A dynamic mode must not bake its ignored static Bresenham value into
    * the sample-frequency decision for the fragment entry point. */
   return nir->info.fs.early_fragment_tests ||
      (state && state->ms && state->rs &&
       (state->rs->line.mode != VK_LINE_RASTERIZATION_MODE_BRESENHAM ||
        BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE)) &&
       state->ms->rasterization_samples > VK_SAMPLE_COUNT_1_BIT);
}

static void
kk_line_fragment(nir_shader *nir, bool expanded,
                 const struct vk_graphics_pipeline_state *state)
{
   if (!kk_line_effects_possible(state)) return;
   nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
   nir_def *one = nir_imm_float(&b, 1);
   nir_def *coverage = one;
   bool dynamic_mode = state &&
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE);
   bool dynamic_stipple = state &&
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE);
   bool smooth = state && state->rs &&
      (state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
       dynamic_mode);
   bool stipple = !kk_line_geometry_stipple(nir, state) && state && state->rs &&
      (state->rs->line.stipple.enable || dynamic_stipple);
   if (expanded && (smooth || stipple)) {
      nir_def *do_smooth = nir_imm_false(&b);
      nir_def *do_stipple = nir_imm_false(&b);
      if (smooth) {
         do_smooth = nir_imm_true(&b);
         if (dynamic_mode) {
            nir_def *mode = kk_polygon_load(&b, 1, 32,
               offsetof(struct poly_geometry_capture, line_mode), 4);
            do_smooth = nir_ieq_imm(&b, mode,
               VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH);
         }
      }
      if (stipple) {
         do_stipple = nir_imm_true(&b);
         if (dynamic_stipple) {
            nir_def *flags = kk_polygon_load(&b, 1, 32,
               offsetof(struct poly_geometry_capture, line_stipple_flags), 4);
            do_stipple = nir_ine_imm(&b,
               nir_iand_imm(&b, flags, KK_LINE_STIPPLE_ENABLED), 0);
         }
      }
      nir_def *is_line = nir_ine_imm(&b, kk_polygon_load(&b, 1, 32,
         offsetof(struct poly_geometry_capture, line_expand), 4), 0);
      nir_push_if(&b, nir_iand(&b, is_line, nir_ior(&b, do_smooth, do_stipple)));
      nir_def *barycentric = nir_load_barycentric_pixel(&b, 32,
         .interp_mode = INTERP_MODE_NOPERSPECTIVE);
      /* The same original endpoints are carried by every emitted vertex,
       * including every stipple span. Reconstruct the full line equation here
       * instead of interpolating a span's rounded tangent or distances. */
      nir_def *endpoints = nir_load_input(&b, 4, 32,
         nir_imm_int(&b, 0), .dest_type = nir_type_float32,
         .io_semantics.location = VARYING_SLOT_FOGC,
         .io_semantics.num_slots = 1);
      nir_def *a = nir_trim_vector(&b, endpoints, 2);
      nir_def *delta = nir_fsub(&b,
         nir_channels(&b, endpoints, 0xc), a);
      /* This helper runs after sample-position lowering. The new intrinsic is
       * the native pixel position: coverage uses the pixel square, including
       * in sample-frequency shaders. Register it for the MSL entry point. */
      BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
      nir_def *pixel = nir_fadd_imm(&b, nir_ffloor(&b,
         nir_trim_vector(&b, nir_load_frag_coord(&b), 2)), 0.5f);
      nir_def *offset = nir_fsub(&b, pixel, a);
      bool static_bresenham = state->rs->line.mode ==
         VK_LINE_RASTERIZATION_MODE_BRESENHAM;
      nir_def *is_bresenham = nir_imm_bool(&b, static_bresenham);
      if (stipple && dynamic_mode) {
         nir_def *mode = kk_polygon_load(&b, 1, 32,
            offsetof(struct poly_geometry_capture, line_mode), 4);
         is_bresenham = nir_ieq_imm(&b, mode,
            VK_LINE_RASTERIZATION_MODE_BRESENHAM);
      }
      nir_def *length = NULL, *tangent = NULL, *along = NULL;
      if (smooth || (stipple && (!static_bresenham || dynamic_mode))) {
         nir_def *dx = nir_channel(&b, delta, 0);
         nir_def *dy = nir_channel(&b, delta, 1);
         nir_def *horizontal = nir_feq_imm(&b, dy, 0);
         nir_def *vertical = nir_feq_imm(&b, dx, 0);
         nir_def *length2 = nir_fdot2(&b, delta, delta);
         length = nir_bcsel(&b, horizontal, nir_fabs(&b, dx),
            nir_bcsel(&b, vertical, nir_fabs(&b, dy), nir_fsqrt(&b, length2)));
         tangent = nir_fdiv(&b, delta,
            nir_fmax(&b, length, nir_imm_float(&b, 1.0e-15f)));
         /* Exact cardinal directions keep integral stipple boundaries exact
          * under reciprocal approximations used by Metal fast math. */
         nir_def *zero = nir_imm_float(&b, 0);
         tangent = nir_bcsel(&b, horizontal,
            nir_vec2(&b, nir_fsign(&b, dx), zero),
            nir_bcsel(&b, vertical,
               nir_vec2(&b, zero, nir_fsign(&b, dy)), tangent));
         along = nir_fdot2(&b, offset, tangent);
      }
      nir_def *line_coverage = one;
      if (smooth) {
         if (dynamic_mode) nir_push_if(&b, do_smooth);
         nir_def *width = kk_polygon_load(&b, 1, 32,
            offsetof(struct poly_geometry_capture, line_width), 4);
         nir_def *perpendicular = nir_fsub(&b,
            nir_fmul(&b, nir_channel(&b, offset, 1), nir_channel(&b, tangent, 0)),
            nir_fmul(&b, nir_channel(&b, offset, 0), nir_channel(&b, tangent, 1)));
         nir_def *edge = nir_fsat(&b, nir_fsub(&b,
            nir_fadd_imm(&b, nir_fmul_imm(&b, width, 0.5f), 0.5f),
            nir_fabs(&b, perpendicular)));
         nir_def *start = nir_fsat(&b, nir_fadd_imm(&b, along, 0.5f));
         nir_def *end = nir_fsat(&b, nir_fadd_imm(&b,
            nir_fsub(&b, length, along), 0.5f));
         line_coverage = nir_fmul(&b, edge, nir_fmul(&b, start, end));
         if (dynamic_mode) {
            nir_pop_if(&b, NULL);
            line_coverage = nir_if_phi(&b, line_coverage, one);
         }
      }
      if (stipple) {
         if (dynamic_stipple) nir_push_if(&b, do_stipple);
         nir_def *factor = kk_polygon_load(&b, 1, 32,
            offsetof(struct poly_geometry_capture, line_stipple_factor), 4);
         nir_def *pattern = kk_polygon_load(&b, 1, 32,
            offsetof(struct poly_geometry_capture, line_stipple_pattern), 4);
         nir_def *packet = nir_load_interpolated_input(&b, 4, 32, barycentric,
            nir_imm_int(&b, 0), .dest_type = nir_type_float32,
            .io_semantics.location = VARYING_SLOT_PNTC,
            .io_semantics.num_slots = 1);
         nir_def *position = along;
         if (static_bresenham || dynamic_mode) {
            /* Bresenham advances once per major-axis pixel, for all samples.
             * The packet prefix accounts for the first diamond-exiting cell. */
            nir_def *cells = nir_fsub(&b, nir_ffloor(&b, pixel), nir_ffloor(&b, a));
            nir_def *signed_cells = nir_fmul(&b, cells,
               nir_bcsel(&b, nir_fge_imm(&b, delta, 0),
                  nir_imm_float(&b, 1), nir_imm_float(&b, -1)));
            nir_def *major = nir_fge(&b,
               nir_fabs(&b, nir_channel(&b, delta, 0)),
               nir_fabs(&b, nir_channel(&b, delta, 1)));
            nir_def *bres_position = nir_bcsel(&b, major,
               nir_channel(&b, signed_cells, 0), nir_channel(&b, signed_cells, 1));
            position = dynamic_mode ? nir_bcsel(&b, is_bresenham,
               bres_position, position) : bres_position;
         }
         if ((!static_bresenham || dynamic_mode) && state->ms &&
             state->ms->rasterization_samples > VK_SAMPLE_COUNT_1_BIT) {
            /* Rectangular stipple removes unit rectangles at sample locations;
             * smooth alpha and Bresenham phase remain pixel based. */
            BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID);
            nir_def *sample_offset = nir_fadd_imm(&b,
               nir_load_sample_pos_from_id(&b, 32, nir_load_sample_id(&b)), -0.5f);
            nir_def *sample_along = nir_fadd(&b, along,
               nir_fdot2(&b, sample_offset, tangent));
            position = dynamic_mode ? nir_bcsel(&b, is_bresenham,
               position, sample_along) : sample_along;
         }
         nir_def *phase = nir_f2u32(&b, nir_ffloor(&b, nir_fmax(&b,
            nir_fadd(&b, nir_channel(&b, packet, 0), position),
            nir_imm_float(&b, 0))));
         nir_def *bit = nir_iand_imm(&b,
            nir_udiv(&b, phase, nir_umax(&b, factor, nir_imm_int(&b, 1))), 15);
         nir_discard_if(&b,
            nir_ieq_imm(&b, nir_iand_imm(&b, nir_ushr(&b, pattern, bit), 1), 0));
         if (dynamic_stipple) nir_pop_if(&b, NULL);
         nir->info.fs.uses_discard = true;
      }
      nir_pop_if(&b, NULL);
      coverage = nir_if_phi(&b, line_coverage, one);
   }
   nir_shader_intrinsics_pass(nir, kk_line_resolve_coverage,
                              nir_metadata_control_flow, coverage);
}

/* Mesh readers carry the original endpoints and width per primitive. Only
 * the selected triangle reader uses this fragment program; native lines keep
 * their original entry point and contain no coverage loads or arithmetic. */
static void
kk_mesh_line_fragment(nir_shader *nir)
{
   nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
   nir_def *ends = nir_load_per_primitive_input(&b, 4, 32, nir_imm_int(&b, 0),
      .dest_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_FOGC,
      .io_semantics.num_slots = 1);
   nir_def *width = nir_load_per_primitive_input(&b, 1, 32, nir_imm_int(&b, 0),
      .dest_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_PNTC,
      .io_semantics.num_slots = 1);
   nir_def *a = nir_trim_vector(&b, ends, 2);
   nir_def *delta = nir_fsub(&b, nir_channels(&b, ends, 0xc), a);
   nir_def *length = nir_fsqrt(&b, nir_fdot2(&b, delta, delta));
   nir_def *tangent = nir_fdiv(&b, delta, nir_fmax(&b, length,
      nir_imm_float(&b, 1.0e-15f)));
   nir_def *pixel = nir_fadd_imm(&b, nir_ffloor(&b,
      nir_trim_vector(&b, nir_load_frag_coord(&b), 2)), 0.5f);
   nir_def *offset = nir_fsub(&b, pixel, a);
   nir_def *along = nir_fdot2(&b, offset, tangent);
   nir_def *perp = nir_fsub(&b,
      nir_fmul(&b, nir_channel(&b, offset, 1), nir_channel(&b, tangent, 0)),
      nir_fmul(&b, nir_channel(&b, offset, 0), nir_channel(&b, tangent, 1)));
   nir_def *edge = nir_fsat(&b, nir_fsub(&b,
      nir_fadd_imm(&b, nir_fmul_imm(&b, width, 0.5f), 0.5f), nir_fabs(&b, perp)));
   nir_def *start = nir_fsat(&b, nir_fadd_imm(&b, along, 0.5f));
   nir_def *end = nir_fsat(&b, nir_fadd_imm(&b, nir_fsub(&b, length, along), 0.5f));
   nir_def *coverage = nir_fmul(&b, edge, nir_fmul(&b, start, end));
   nir_shader_intrinsics_pass(nir, kk_line_resolve_coverage,
      nir_metadata_control_flow, coverage);
   nir->info.inputs_read |= VARYING_BIT_FOGC | VARYING_BIT_PNTC;
   nir->info.per_primitive_inputs |= VARYING_BIT_FOGC | VARYING_BIT_PNTC;
   BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
}

static bool
kk_line_remove_early_exports(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output) return false;
   unsigned slot = nir_intrinsic_io_semantics(intr).location;
   if (slot != FRAG_RESULT_DEPTH && slot != FRAG_RESULT_STENCIL) return false;
   nir_instr_remove(&intr->instr);
   return true;
}

static void
kk_line_lower_pure_early(nir_shader *nir,
                         const struct vk_graphics_pipeline_state *state)
{
   bool stipple = state && state->rs &&
      (state->rs->line.stipple.enable ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE));
   if (!nir->info.fs.early_fragment_tests) return;
   /* Vulkan ignores these shader exports with forced early tests. Removing
    * the early attribute must not make either ignored export effective. */
   if (nir->info.outputs_written & (BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
                                    BITFIELD64_BIT(FRAG_RESULT_STENCIL)))
      nir_shader_intrinsics_pass(nir, kk_line_remove_early_exports,
                                 nir_metadata_control_flow, NULL);
   nir->info.outputs_written &= ~(BITFIELD64_BIT(FRAG_RESULT_DEPTH) |
                                  BITFIELD64_BIT(FRAG_RESULT_STENCIL));
   if (!stipple) return;
   bool observable = nir->info.writes_memory || nir->info.fs.post_depth_coverage ||
      nir->info.fs.uses_discard ||
      (nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK)) ||
      (state->ms && state->ms->alpha_to_coverage_enable) ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE);
   if (!observable)
      nir->info.fs.early_fragment_tests = false;
}
