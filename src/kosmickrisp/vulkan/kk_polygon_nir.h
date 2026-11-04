/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT
 * Point-square expansion uses Mesa draw_pipe_wide_point.c's two-triangle
 * layout after clipping; all polygon attributes remain constant per point. */

static bool
kk_polygon_possible(const struct kk_device *dev, const struct vk_graphics_pipeline_state *state)
{
   return state && state->rs &&
      (state->rs->polygon_mode == VK_POLYGON_MODE_POINT ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE) ||
       (dev->vk.enabled_features.depthBiasControl &&
        (state->rs->depth_bias.enable ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) &&
        (state->rs->depth_bias.exact || state->rs->depth_bias.representation !=
            VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS))));
}

static uint64_t
kk_polygon_noperspective_outputs(nir_shader *nir)
{
   uint64_t mask = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_interpolated_input) continue;
            nir_intrinsic_instr *barycentric = nir_src_as_intrinsic(intr->src[0]);
            if (nir_intrinsic_interp_mode(barycentric) == INTERP_MODE_NOPERSPECTIVE)
               mask |= BITFIELD64_BIT(nir_intrinsic_io_semantics(intr).location);
         }
      }
   }
   return mask;
}

/* A synthetic GS must carry the input polygon's ID through point expansion.
 * Native primitive_id would number the generated point-square triangles. */
static bool
kk_polygon_passthrough_primitive_id(nir_builder *b, nir_intrinsic_instr *intr,
                                   void *data)
{
   if (intr->intrinsic != nir_intrinsic_emit_vertex)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_store_output(b, nir_load_primitive_id(b), nir_imm_int(b, 0),
      .src_type = nir_type_uint32,
      .io_semantics.location = VARYING_SLOT_PRIMITIVE_ID,
      .io_semantics.num_slots = 1);
   b->shader->info.outputs_written |= VARYING_BIT_PRIMITIVE_ID;
   return true;
}

static nir_def *
kk_polygon_data(nir_builder *b)
{
   nir_def *address = nir_iadd_imm(b, nir_load_geometry_param_buffer_poly(b),
      offsetof(struct poly_geometry_params, capture));
   return nir_load_global_constant(b, 1, 64, address, .align_mul = 8);
}

static nir_def *
kk_polygon_load(nir_builder *b, unsigned components, unsigned bits,
                 unsigned offset, unsigned alignment)
{
   return nir_load_global_constant(b, components, bits,
      nir_iadd_imm(b, kk_polygon_data(b), offset), .align_mul = alignment);
}

static bool
kk_polygon_point_coord(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_point_coord)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *barycentric = nir_load_barycentric_pixel(b, 32,
      .interp_mode = INTERP_MODE_NOPERSPECTIVE);
   nir_def *coord = nir_load_interpolated_input(b, 2, 32, barycentric,
      nir_imm_int(b, 0), .dest_type = nir_type_float32,
      .io_semantics.location = VARYING_SLOT_PNTC,
      .io_semantics.num_slots = 1);
   nir_def_replace(&intr->def, coord);
   return true;
}

static nir_shader *
kk_polygon_vertex_shader(const struct poly_gs_capture_info *info,
                          const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                  "Poly polygon points");
   nir_shader *nir = b.shader;
   nir->info.io_lowered = true;
   nir->info.outputs_written = (info->outputs & ~VARYING_BIT_PSIZ) | VARYING_BIT_PNTC;
   nir_def *id = nir_load_vertex_id(&b);
   nir_def *mode = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, polygon_mode), 4);
   nir_def *is_point = nir_ieq_imm(&b, mode, VK_POLYGON_MODE_POINT);
   nir_def *point = nir_bcsel(&b, is_point, nir_udiv_imm(&b, id, 6), id);
   nir_def *in_point = nir_umod_imm(&b, id, 6);
   unsigned slots = util_bitcount64(info->outputs);
   nir_def *address = nir_iadd(&b,
      kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
      nir_u2u64(&b, nir_imul_imm(&b, point, (slots + 1) * 16)));
   nir_def *front = nir_load_global_constant(&b, 1, 32,
      nir_iadd_imm(&b, address, slots * 16), .align_mul = 16);
   nir_def *front_ccw = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, front_ccw), 4);
   nir_def *positive = nir_ine(&b, front, front_ccw);
   /* Corners TL, BL, TR, BR; reverse both triangles for the back face. */
   nir_def *corner = nir_imm_int(&b, 0);
   const unsigned corners[6] = {0, 2, 3, 0, 3, 1};
   nir_def *ordered = nir_bcsel(&b, positive, in_point,
      nir_iadd(&b, nir_imul_imm(&b, nir_udiv_imm(&b, in_point, 3), 3),
         nir_bcsel(&b, nir_ieq_imm(&b, nir_umod_imm(&b, in_point, 3), 0),
            nir_imm_int(&b, 0),
            nir_isub(&b, nir_imm_int(&b, 3), nir_umod_imm(&b, in_point, 3)))));
   for (unsigned i = 0; i < ARRAY_SIZE(corners); ++i)
      corner = nir_bcsel(&b, nir_ieq_imm(&b, ordered, i),
                         nir_imm_int(&b, corners[i]), corner);
   nir_def *coord = nir_vec2(&b, nir_u2f32(&b, nir_ushr_imm(&b, corner, 1)),
                              nir_u2f32(&b, nir_iand_imm(&b, corner, 1)));
   nir_def *viewport = nir_imm_int(&b, 0);
   if (info->outputs & VARYING_BIT_VIEWPORT) {
      unsigned slot = util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_VIEWPORT));
      viewport = nir_load_global_constant(&b, 1, 32,
         nir_iadd_imm(&b, address, slot * 16), .align_mul = 16);
   }
   nir_def *viewport_address = nir_iadd(&b, kk_polygon_data(&b),
      nir_u2u64(&b, nir_iadd_imm(&b, nir_imul_imm(&b, viewport, 6 * 4),
         offsetof(struct poly_geometry_capture, viewport))));
   nir_def *vp = nir_load_global_constant(&b, 4, 32, viewport_address, .align_mul = 4);
   nir_def *render_size = kk_polygon_load(&b, 2, 32,
      offsetof(struct poly_geometry_capture, render_size), 4);

   unsigned packed = 0;
   u_foreach_bit64(slot, info->outputs) {
      nir_def *value = nir_load_global_constant(&b, 4, 32,
         nir_iadd_imm(&b, address, packed++ * 16), .align_mul = 16);
      if (slot == VARYING_SLOT_PSIZ)
         continue;
      if (slot == VARYING_SLOT_POS) {
         nir_def *w = nir_channel(&b, value, 3);
         nir_def *ndc = nir_fdiv(&b, nir_channels(&b, value, 3), w);
         nir_def *screen = nir_ffma(&b,
            nir_fmul_imm(&b, nir_fadd_imm(&b, ndc, 1), 0.5f),
            nir_channels(&b, vp, 12), nir_channels(&b, vp, 3));
         screen = nir_fadd(&b, screen, nir_bcsel(&b, is_point,
            nir_fadd_imm(&b, coord, -0.5f), nir_imm_vec2(&b, 0, 0)));
         nir_def *position = nir_fmul(&b,
            nir_fadd_imm(&b, nir_fmul_imm(&b, nir_fdiv(&b, screen, render_size), 2), -1), w);
         value = nir_vec4(&b, nir_channel(&b, position, 0),
            nir_channel(&b, position, 1), nir_channel(&b, value, 2), w);
      } else {
         value = nir_convert_to_bit_size(&b, value,
            nir_alu_type_get_base_type(info->output_types[slot]),
            nir_alu_type_get_type_size(info->output_types[slot]));
      }
      nir_store_output(&b, nir_trim_vector(&b, value, info->output_components[slot]),
         nir_imm_int(&b, 0), .src_type = info->output_types[slot],
         .io_semantics.location = slot, .io_semantics.num_slots = 1);
   }
   nir_store_output(&b, coord, nir_imm_int(&b, 0), .src_type = nir_type_float32,
      .io_semantics.location = VARYING_SLOT_PNTC, .io_semantics.num_slots = 1);
   /* Clipping and cull-distance rejection happened on original triangles.
    * Keep their varyings, but do not clip the expanded squares again. */
   nir->info.clip_distance_array_size = 0;
   nir->info.cull_distance_array_size = 0;
   return nir;
}
