/* SPDX-License-Identifier: MIT */
nir_def *
kk_conservative_evaluate_inner_coverage(nir_builder *b, nir_def *packet,
                                       nir_def *rectangle)
{
   nir_variable *result = nir_local_variable_create(b->impl, glsl_bool_type(),
                                                    "conservative_fully_covered");
   nir_store_var(b, result, nir_imm_false(b), 1);
   nir_def *edges = cr_load(b, packet, 1, 64, inner_edges);
   nir_def *count = cr_load(b, packet, 1, 32, count);
   nir_def *valid = nir_iand(b, nir_ine_imm(b, edges, 0),
      nir_ieq_imm(b, cr_load(b, packet, 1, 32, degenerate), 0));
   nir_push_if(b, valid);
   {
      nir_store_var(b, result, nir_imm_true(b), 1);
      nir_variable *index = nir_local_variable_create(b->impl, glsl_uint_type(),
                                                       "inner_edge");
      nir_store_var(b, index, nir_imm_int(b, 0), 1);
      nir_push_loop(b);
      nir_def *i = nir_load_var(b, index);
      nir_break_if(b, nir_uge(b, i, count));
      nir_def *address = nir_iadd(b, edges, nir_u2u64(b,
         nir_imul_imm(b, i, sizeof(struct kk_conservative_inner_edge))));
      nir_def *A = nir_load_global_constant(b, 1, 64, address, .align_mul = 8);
      nir_def *B = nir_load_global_constant(b, 1, 64,
         nir_iadd_imm(b, address, 8), .align_mul = 8);
      nir_def *C = nir_load_global_constant(b, 1, 64,
         nir_iadd_imm(b, address, 16), .align_mul = 8);
      nir_def *x = nir_bcsel(b, nir_ilt_imm(b, A, 0),
         nir_channel(b, rectangle, 2), nir_channel(b, rectangle, 0));
      nir_def *y = nir_bcsel(b, nir_ilt_imm(b, B, 0),
         nir_channel(b, rectangle, 3), nir_channel(b, rectangle, 1));
      x = nir_imul_imm(b, nir_i2i64(b, x), 256);
      y = nir_imul_imm(b, nir_i2i64(b, y), 256);
      nir_def *minimum = nir_iadd(b,
         nir_iadd(b, nir_imul(b, A, x), nir_imul(b, B, y)), C);
      nir_def *norm = nir_iadd(b, nir_iabs(b, A), nir_iabs(b, B));
      nir_push_if(b, nir_ilt(b, nir_imul_imm(b, minimum, 2), norm));
      {
         nir_store_var(b, result, nir_imm_false(b), 1);
         nir_jump(b, nir_jump_break);
      }
      nir_pop_if(b, NULL);
      nir_store_var(b, index, nir_iadd_imm(b, i, 1), 1);
      nir_pop_loop(b, NULL);
   }
   nir_pop_if(b, NULL);
   return nir_load_var(b, result);
}

static bool
kk_conservative_disabled_inner(nir_builder *b, nir_intrinsic_instr *intr,
                                 void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_fully_covered)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_imm_false(b));
   return true;
}
