/* SPDX-License-Identifier: MIT */

/* Called once in original sequence order after token/count/predicate expansion.
 * kk_draw keeps the original per-view producer semantics, then its existing
 * kk_poly_layer_dispatch_draw runs only retained raster readers. The Layer
 * callback binds actual logical slices between capture and those readers.
 * No original user-stage ICB is submitted here or from the layer loop. */
void
kk_dgc_execute_layered_sequence(struct kk_cmd_buffer *cmd, uint64_t root,
   uint64_t args, uint64_t binding, bool indexed, uint32_t draw_id)
{
   assert(cmd->state.gfx.render.layered_bc);
   kk_dgc_draw_sequence(cmd, root, args, binding, indexed, draw_id);
   /* The next sequence may reset the same Poly heap, and the next expansion
    * slice may overwrite scratch. End the last raster encoder before either
    * producer dispatch; command/queue barriers order all readers before reuse.
    * Pool uploads and attachment/native objects keep command-owned lifetime. */
   cs_get_compute(cmd);
}
