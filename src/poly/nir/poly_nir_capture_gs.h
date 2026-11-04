/*
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT
 *
 * Uses the original Poly input assembly and XFB lowering. Capture the original
 * geometry invocation once, before polygon clipping changes its output.
 */

struct capture_gs_state {
   struct lower_gs_state *gs;
   struct lower_output_to_var_slot outputs[NUM_TOTAL_VARYING_SLOTS];
};

static nir_def *
capture_param(nir_builder *b, unsigned offset, unsigned bytes)
{
   nir_def *address = nir_iadd_imm(b, load_geometry_param(b, capture), offset);
   return nir_load_global(b, 1, bytes * 8, address, .align_mul = bytes);
}

#define CAPTURE_PARAM(b, field) \
   capture_param(b, offsetof(struct poly_geometry_capture, field), \
                 sizeof(((struct poly_geometry_capture *)0)->field))

static nir_def *
capture_vertex_index(nir_builder *b, nir_def *invocation, nir_def *vertex,
                      nir_def *stream)
{
   nir_def *stream_index = nir_iadd(
      b, nir_imul(b, invocation, CAPTURE_PARAM(b, streams)), stream);
   return nir_iadd(b, nir_imul(b, stream_index, CAPTURE_PARAM(b, vertices_out)),
                   vertex);
}

static nir_def *
capture_vertex_address(nir_builder *b, nir_def *index, unsigned slot)
{
   uint64_t preceding = b->shader->info.outputs_written & BITFIELD64_MASK(slot);
   nir_def *offset = nir_iadd_imm(
      b, nir_imul(b, index, CAPTURE_PARAM(b, vertex_stride)),
      util_bitcount64(preceding) * 16);
   return nir_iadd(b, CAPTURE_PARAM(b, vertices), nir_u2u64(b, offset));
}

static bool
capture_gs_intrinsic(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct capture_gs_state *state = data;
   b->cursor = nir_before_instr(&intr->instr);

   switch (intr->intrinsic) {
   case nir_intrinsic_store_output:
      lower_store_to_var(b, intr, state->outputs);
      return true;

   case nir_intrinsic_select_vertex_poly: {
      nir_def *stream = nir_imm_int(b, nir_intrinsic_stream_id(intr));
      nir_def *invocation = calc_unrolled_id(b);
      nir_def *vertex = intr->src[0].ssa;
      nir_def *index = capture_vertex_index(b, invocation, vertex, stream);

      u_foreach_bit64(slot, b->shader->info.outputs_written) {
         struct lower_output_to_var_slot *output = &state->outputs[slot];
         nir_def *value = nir_load_var(b, output->output_var);
         value = nir_convert_to_bit_size(
            b, value, nir_alu_type_get_base_type(output->type), 32);
         value = nir_pad_vector_imm_int(b, value, 0, 4);
         nir_store_global(b, value, capture_vertex_address(b, index, slot));
      }
      nir_def *count_index = nir_iadd(
         b, nir_imul(b, invocation, CAPTURE_PARAM(b, streams)), stream);
      nir_def *count_address = nir_iadd(
         b, CAPTURE_PARAM(b, counts), nir_u2u64(b, nir_imul_imm(b, count_index, 4)));
      nir_store_global(b, nir_iadd_imm(b, vertex, 1), count_address);
      nir_instr_remove(&intr->instr);
      return true;
   }

   case nir_intrinsic_emit_primitive_poly: {
      /* Every captured vertex remembers its original strip for XFB and
       * post-assembly triangle decomposition. */
      nir_def *first = intr->src[1].ssa;
      nir_def *length = intr->src[2].ssa;
      nir_variable *i = nir_local_variable_create(b->impl, glsl_uint_type(), NULL);
      nir_store_var(b, i, nir_imm_int(b, 0), 1);
      nir_push_loop(b);
      nir_def *id = nir_load_var(b, i);
      nir_break_if(b, nir_uge(b, id, length));
      nir_def *index = capture_vertex_index(
         b, calc_unrolled_id(b), nir_iadd(b, first, id),
         nir_imm_int(b, nir_intrinsic_stream_id(intr)));
      nir_def *address = nir_iadd(
         b, CAPTURE_PARAM(b, strips), nir_u2u64(b, nir_imul_imm(b, index, 16)));
      nir_store_global(b, nir_vec4(b, first, length, intr->src[3].ssa, id), address);
      unsigned primitive_vertices =
         mesa_vertices_per_prim(u_reduced_prim(b->shader->info.gs.output_primitive));
      nir_push_if(b, nir_uge_imm(b, id, primitive_vertices - 1));
      {
         nir_def *primitive = nir_iadd_imm(b, id, 1 - primitive_vertices);
         if (primitive_vertices <= 2) {
            nir_def *base = nir_iadd(b,
               nir_imul(b, calc_unrolled_id(b), CAPTURE_PARAM(b, streams)),
               nir_imm_int(b, nir_intrinsic_stream_id(intr)));
            nir_def *index = nir_iadd(b,
               nir_imul(b, base, nir_iadd_imm(b, CAPTURE_PARAM(b, vertices_out), 1 - primitive_vertices)),
               nir_iadd(b, intr->src[3].ssa, primitive));
            nir_def *address = nir_iadd(b, CAPTURE_PARAM(b, triangles),
               nir_u2u64(b, nir_imul_imm(b, index, 16)));
            nir_def *a = nir_iadd(b, first, primitive);
            nir_def *c = nir_iadd_imm(b, a, primitive_vertices - 1);
            nir_store_global(b, nir_vec4(b, a, c, c, intr->src[3].ssa), address);
         } else {
         nir_def *odd = nir_ine_imm(b, nir_iand_imm(b, primitive, 1), 0);
         nir_def *last = nir_i2b(b, nir_load_provoking_last(b));
         nir_def *a = nir_iadd(b, primitive,
            nir_b2i32(b, nir_iand(b, odd, last)));
         nir_def *middle = nir_bcsel(b, odd,
            nir_bcsel(b, last, nir_imm_int(b, 0), nir_imm_int(b, 2)),
            nir_imm_int(b, 1));
         nir_def *c = nir_iadd(b, primitive,
            nir_bcsel(b, nir_iand(b, odd, nir_inot(b, last)),
                      nir_imm_int(b, 1), nir_imm_int(b, 2)));
         nir_def *triangle_base = nir_iadd(b,
            nir_imul(b, calc_unrolled_id(b), CAPTURE_PARAM(b, streams)),
            nir_imm_int(b, nir_intrinsic_stream_id(intr)));
         nir_def *triangle_index = nir_iadd(b,
            nir_imul(b, triangle_base, nir_iadd_imm(b, CAPTURE_PARAM(b, vertices_out), -2)),
            nir_iadd(b, intr->src[3].ssa, primitive));
         nir_def *triangle_address = nir_iadd(b, CAPTURE_PARAM(b, triangles),
            nir_u2u64(b, nir_imul_imm(b, triangle_index, 16)));
         nir_store_global(b, nir_vec4(b, nir_iadd(b, first, a),
            nir_iadd(b, first, nir_iadd(b, primitive, middle)),
            nir_iadd(b, first, c), nir_imm_int(b, 0)), triangle_address);
         }
      }
      nir_pop_if(b, NULL);
      nir_store_var(b, i, nir_iadd_imm(b, id, 1), 1);
      nir_pop_loop(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   }

   case nir_intrinsic_set_vertex_and_primitive_count: {
      write_xfb_counts(b, intr, state->gs);
      unsigned stream = nir_intrinsic_stream_id(intr);
      if (stream < stream_multiplier(b->shader)) {
         nir_def *index = nir_iadd_imm(b,
            nir_imul(b, calc_unrolled_id(b), CAPTURE_PARAM(b, streams)), stream);
         nir_def *address = nir_iadd(b, CAPTURE_PARAM(b, primitive_counts),
            nir_u2u64(b, nir_imul_imm(b, index, 4)));
         nir_store_global(b, intr->src[2].ssa, address);
      }
      nir_instr_remove(&intr->instr);
      return true;
   }

   default:
      return false;
   }
}

static nir_shader *
create_gs_capture_shader(const nir_shader *gs, struct lower_gs_state *gs_state,
                         struct poly_gs_capture_info *info)
{
   nir_shader *shader = nir_shader_clone(NULL, gs);
   struct capture_gs_state state = {.gs = gs_state};
   nir_shader_intrinsics_pass(shader, collect_output_types, nir_metadata_all,
                              state.outputs);
   *info = (struct poly_gs_capture_info){
      .outputs = gs->info.outputs_written,
      .vertices_out = gs->info.gs.vertices_out,
      .primitive_vertices = mesa_vertices_per_prim(u_reduced_prim(gs->info.gs.output_primitive)),
      .streams = stream_multiplier(gs),
      .clip_count = gs->info.clip_distance_array_size,
      .cull_count = gs->info.cull_distance_array_size,
   };
   u_foreach_bit64(slot, info->outputs) {
      struct lower_output_to_var_slot *output = &state.outputs[slot];
      enum glsl_base_type type = nir_get_glsl_base_type_for_nir_type(output->type);
      output->output_var = nir_variable_create(
         shader, nir_var_shader_temp,
         glsl_vector_type(type, output->nr_components), NULL);
      info->output_types[slot] = output->type;
      info->output_components[slot] = output->nr_components;
      if (nir_alu_type_get_base_type(output->type) == nir_type_float)
         info->float_outputs |= BITFIELD64_BIT(slot);
   }

   nir_builder b = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(shader)));
   for (unsigned stream = 0; stream < info->streams; ++stream) {
      nir_def *index = nir_iadd_imm(
         &b, nir_imul_imm(&b, calc_unrolled_id(&b), info->streams), stream);
      nir_def *address = nir_iadd(
         &b, CAPTURE_PARAM(&b, counts), nir_u2u64(&b, nir_imul_imm(&b, index, 4)));
      nir_store_global(&b, nir_imm_int(&b, 0), address);
   }
   nir_shader_intrinsics_pass(shader, capture_gs_intrinsic,
                              nir_metadata_control_flow, &state);
   nir_shader_intrinsics_pass(shader, lower_id, nir_metadata_control_flow, NULL);
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_cf_list body;
   nir_cf_extract(&body, nir_before_block(nir_start_block(impl)),
                  nir_after_block(nir_impl_last_block(impl)));
   nir_builder guard = nir_builder_at(nir_before_impl(impl));
   nir_def *id = nir_load_global_invocation_id(&guard, 32);
   nir_def *valid = nir_iand(&guard,
      nir_ult(&guard, nir_channel(&guard, id, 0), load_geometry_param(&guard, grid[0])),
      nir_ult(&guard, nir_channel(&guard, id, 1), load_geometry_param(&guard, grid[1])));
   nir_push_if(&guard, valid);
   guard.cursor = nir_cf_reinsert(&body, guard.cursor);
   nir_pop_if(&guard, NULL);
   nir_progress(true, impl, nir_metadata_none);
   return shader;
}

static nir_shader *
create_gs_capture_xfb_shader(const nir_shader *gs,
                             struct lower_gs_state *state)
{
   if (!gs->xfb_info)
      return NULL;
   nir_builder b_ = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      gs->options, "Poly captured geometry XFB");
   nir_builder *b = &b_;
   nir_shader *shader = b->shader;
   shader->info.outputs_written = gs->info.outputs_written;
   nir_def *id = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def *invocation = nir_channel(b, nir_load_global_invocation_id(b, 32), 1);
   unsigned streams = stream_multiplier(gs);
   nir_def *stream = nir_umod_imm(b, id, streams);
   nir_def *vertex = nir_udiv_imm(b, id, streams);
   nir_def *index = capture_vertex_index(b, invocation, vertex, stream);
   nir_def *count_index = nir_iadd(b, nir_imul_imm(b, invocation, streams), stream);
   nir_def *count_address = nir_iadd(
      b, CAPTURE_PARAM(b, counts), nir_u2u64(b, nir_imul_imm(b, count_index, 4)));
   nir_def *count = nir_load_global(b, 1, 32, count_address, .align_mul = 4);
   nir_push_if(b, nir_ult(b, vertex, count));

   nir_def *metadata = nir_load_global(b, 4, 32,
      nir_iadd(b, CAPTURE_PARAM(b, strips), nir_u2u64(b, nir_imul_imm(b, index, 16))),
      .align_mul = 16);
   struct lower_gs_rast_state rs = {
      .points = false,
      .stream = stream,
      .output_id = vertex,
      .instance_id = nir_udiv(b, invocation, load_geometry_param(b, grid[0])),
      .primitive_id = nir_umod(b, invocation, load_geometry_param(b, grid[0])),
      .output_strip_length = nir_local_variable_create(b->impl, glsl_uint_type(), NULL),
      .output_strip_base = nir_local_variable_create(b->impl, glsl_uint_type(), NULL),
      .id_in_strip = nir_local_variable_create(b->impl, glsl_uint_type(), NULL),
   };
   nir_store_var(b, rs.output_strip_length, nir_channel(b, metadata, 1), 1);
   nir_store_var(b, rs.output_strip_base, nir_channel(b, metadata, 2), 1);
   nir_store_var(b, rs.id_in_strip, nir_channel(b, metadata, 3), 1);
   for (unsigned i = 0; i < gs->xfb_info->output_count; ++i) {
      unsigned slot = gs->xfb_info->outputs[i].location;
      if (rs.outputs[slot].selected_var)
         continue;
      nir_variable *var = nir_local_variable_create(b->impl, glsl_uvec4_type(), NULL);
      rs.outputs[slot].selected_var = var;
      nir_def *value = nir_load_global(b, 4, 32, capture_vertex_address(b, index, slot),
                                       .align_mul = 16);
      nir_store_var(b, var, value, 0xf);
   }

   if (gs->xfb_info) {
      unsigned n_ = mesa_vertices_per_prim(gs->info.gs.output_primitive);
      nir_def *zero = nir_imm_int(b, 0);
      nir_def *strip_length =
         rs.points ? zero : nir_load_var(b, rs.output_strip_length);
      nir_def *id_in_strip = rs.points ? zero : nir_load_var(b, rs.id_in_strip);
      nir_def *base =
         rs.points ? rs.output_id : nir_load_var(b, rs.output_strip_base);

      struct nir_xfb_info *xfb = gs->xfb_info;

      nir_def *unrolled = nir_iadd(
         b, nir_imul(b, rs.instance_id, load_geometry_param(b, grid[0])),
         rs.primitive_id);

      nir_def *n = nir_imm_int(b, n_);

      for (unsigned p_ = 0; p_ < n_; ++p_) {
         nir_def *p = nir_imm_int(b, p_);
         nir_push_if(b, poly_xfb_vertex_copy_in_strip(b, n, id_in_strip,
                                                      strip_length, p));

         /* Write XFB for each output */
         for (unsigned i = 0; i < xfb->output_count; ++i) {
            nir_xfb_output_info output = xfb->outputs[i];
            unsigned stream = xfb->buffer_to_stream[output.buffer];
            nir_push_if(b, nir_ieq_imm(b, rs.stream, stream));

            /* Get the index of this primitive in the XFB buffer. That is, the
             * base for this invocation for the stream plus the offset within
             * this invocation.
             */
            nir_def *invocation_base = poly_previous_xfb_primitives(
               b, nir_load_geometry_param_buffer_poly(b),
               nir_imm_int(b, state->static_count[stream]),
               nir_imm_int(b, state->count_index[stream]),
               nir_imm_int(b, state->info->count_words),
               nir_imm_bool(b, state->info->prefix_sum), unrolled);

            nir_def *index = poly_xfb_vertex_offset(
               b, n, invocation_base, base, id_in_strip, p,
               nir_inot(b, nir_i2b(b, nir_load_provoking_last(b))));

            nir_def *xfb_verts = load_geometry_param(b, xfb_verts[stream]);
            nir_push_if(b, nir_ult(b, index, xfb_verts));
            {
               unsigned buffer = output.buffer;
               unsigned stride = xfb->buffers[buffer].stride;

               nir_variable *var = rs.outputs[output.location].selected_var;
               nir_def *value =
                  var ? nir_load_var(b, var) : nir_undef(b, 4, 32);

               /* In case output.component_mask contains invalid components,
                * write out zeroes instead of blowing up validation.
                *
                * KHR-Single-GL44.enhanced_layouts.xfb_capture_inactive_output_component
                * hits this.
                */
               value = nir_pad_vector_imm_int(b, value, 0, 4);

               nir_def *addr = poly_xfb_vertex_address(
                  b, nir_load_geometry_param_buffer_poly(b), index,
                  nir_imm_int(b, buffer), nir_imm_int(b, stride),
                  nir_imm_int(b, output.offset));

               nir_store_global(
                  b, nir_channels(b, value, output.component_mask), addr);
            }
            nir_pop_if(b, NULL);
            nir_pop_if(b, NULL);
         }
         nir_pop_if(b, NULL);
      }
   }



   nir_pop_if(b, NULL);
   shader->info.outputs_written = 0;
   return shader;
}
