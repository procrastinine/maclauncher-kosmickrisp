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
   /* Specialize the complete counter region away when no query is active.
    * Keep it in NIR so captured fragment bodies retain the same guard. */
   nir_push_if(b, nir_load_pipeline_statistics_enabled_kk(b));
   nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);
   nir_def *addr = nir_build_load_global_constant(b, 1, 64, nir_iadd_imm(b, root, offsetof(struct kk_root_descriptor_table, draw.fragment_statistic)), .align_mul = 8);
   nir_def *enabled = nir_ine_imm(b, addr, 0);
   /* Keep the driver query side effect distinct from application writes.
    * Generic helper/demote lowering must not add a helper-status input merely
    * because this entry is counted; Metal suppresses native helper atomics. */
   nir_push_if(b, enabled);
   nir_increment_pipeline_statistic_kk(b, addr);
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
   return nir_progress(true, b->impl, nir_metadata_control_flow);
}
