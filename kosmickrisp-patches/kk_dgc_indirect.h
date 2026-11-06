/* SPDX-License-Identifier: MIT */
/* Bounded recording for the Poly, restart and query paths. Each batch keeps
 * the API's global DrawIndex, and predicates against count minus its first
 * draw; uploaded indirect buffers never multiply an unbounded 32-bit count. */
static void kk_draw_large_indirect(struct kk_cmd_buffer *cmd,
                                   const struct kk_draw_command *original)
{
   uint32_t previous = cmd->state.gfx.indirect_draw_id;
   for (uint32_t first = 0; first < original->draw_count; first += 4096) {
      struct kk_draw_command batch = *original;
      batch.draw_count = MIN2(original->draw_count - first, 4096);
      batch.indirect_command.addr =
          original->indirect_command.addr +
          (uint64_t)first * original->indirect_command.stride;
      if (batch.predicate_count &&
          batch.predicate_op[0] == KK_PREDICATE_GT_DRAW_ID) {
         batch.predicate_addr[0] =
             kk_dgc_offset_count(cmd, original->predicate_addr[0], first);
         if (!batch.predicate_addr[0])
            break;
      }
      cmd->state.gfx.indirect_draw_id = first;
      kk_draw(cmd, &batch);
   }
   cmd->state.gfx.indirect_draw_id = previous;
}
