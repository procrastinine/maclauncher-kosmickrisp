// SPDX-License-Identifier: MIT
// Order timestamp reset after already encoded counter resolves
export const id = "order-timestamp-reset-after-already-encoded-counter-resolves";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_query_pool.c", source => source
    .replace("   if (util_dynarray_num_elements(&cmd->ts_resolves, struct kk_ts_resolve) > 0)\n      cs_end(cmd);\n\n   emit_zero_queries(cmd, pool, firstQuery, queryCount, false);\n}\n\n",
      () => "   if (util_dynarray_num_elements(&cmd->ts_resolves, struct kk_ts_resolve) > 0)\n      cs_end(cmd);\n\n   /* Resolves can already be encoded while their BLIT writes are still in\n    * flight. Reset must happen after them even when ts_resolves is now empty. */\n   if (kk_pool_is_ts(pool) && (cmd->closed_encoder_stages & MTL_STAGE_BLIT))\n      mtl_barrier_after_queue_stages(cs_get_compute(cmd), MTL_STAGE_BLIT,\n                                     MTL_STAGE_DISPATCH);\n\n   emit_zero_queries(cmd, pool, firstQuery, queryCount, false);\n}\n\n"));
}
