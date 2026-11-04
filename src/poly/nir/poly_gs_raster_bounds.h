/* SPDX-License-Identifier: MIT */
/* A generated index reserves power-of-two slots for each input primitive.
 * Restart values and holes in that encoding must not become buffer reads. */
static void
poly_guard_gs_raster(nir_shader *shader, const nir_shader *gs,
                     const struct lower_gs_rast_state *rs,
                     nir_cursor body_start)
{
   nir_builder b = nir_builder_at(body_start);
   nir_def *valid = nir_iand(
      &b, nir_ult(&b, rs->primitive_id, load_geometry_param(&b, grid[0])),
      nir_ult(&b, rs->instance_id, load_geometry_param(&b, grid[1])));
   valid = nir_iand(&b, valid,
      nir_ult_imm(&b, rs->output_id, gs->info.gs.vertices_out));

   nir_cf_list body;
   nir_cf_extract(&body, b.cursor, nir_after_impl(b.impl));
   b.cursor = nir_after_def(valid);
   nir_if *gate = nir_push_if(&b, valid);
   b.cursor = nir_cf_reinsert(&body, b.cursor);
   nir_push_else(&b, gate);
   u_foreach_bit64(slot, shader->info.outputs_written) {
      const struct lower_output_to_var_slot *output = &rs->outputs[slot];
      nir_alu_type type = output->type;
      nir_def *value;
      if (slot == VARYING_SLOT_PSIZ && type == nir_type_invalid) {
         /* The default point-size pass runs after output collection. */
         type = nir_type_float32;
         value = nir_imm_float(&b, 1.0);
      } else {
         value = slot == VARYING_SLOT_POS
            ? nir_imm_vec4(&b, 2.0, 2.0, 2.0, 1.0)
            : nir_imm_zero(&b, output->nr_components,
                           nir_alu_type_get_type_size(type));
      }
      nir_store_output(&b, value, nir_imm_int(&b, 0),
                       .src_type = type,
                       .io_semantics.location = slot);
   }
   nir_pop_if(&b, gate);
   nir_progress(true, b.impl, nir_metadata_none);
}
