/* Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 * Invocation counting follows the shared query model. Metal ignores resource
 * atomics from helper threads; no helper-status or coverage input is needed.
 */
static bool
kk_nir_statistics_fs(nir_shader *s)
{
   nir_builder b_ = nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(s)));
   nir_builder *b = &b_;
   nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);
   nir_def *addr = nir_build_load_global_constant(b, 1, 64, nir_iadd_imm(b, root, offsetof(struct kk_root_descriptor_table, draw.fragment_statistic)), .align_mul = 8);
   nir_def *enabled = nir_ine_imm(b, addr, 0);
   /* Run after application helper/demote lowering. Counting an entry must not
    * alter that lowering or introduce a new fragment coverage input. */
   nir_push_if(b, enabled);
   nir_def *samples = nir_imm_int(b, 1);
   nir_def *old = nir_global_atomic(b, 32, addr, samples, .atomic_op = nir_atomic_op_iadd);
   nir_push_if(b, nir_ult(b, nir_iadd(b, old, samples), old));
   nir_global_atomic(b, 32, nir_iadd_imm(b, addr, 4), nir_imm_int(b, 1), .atomic_op = nir_atomic_op_iadd);
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
   return nir_progress(true, b->impl, nir_metadata_control_flow);
}
