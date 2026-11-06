// SPDX-License-Identifier: MIT
export const id = 'multiview-draw-records';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
`   /* As in Honeykrisp, replay actual work for each view. A one-bit mask keeps
    * every real query producer at one view per replay; EndQuery publishes the
    * summed first query plus the reset zero entries for the other views. */
   u_foreach_bit(view, view_mask) {
      gfx->render.view_mask = BITFIELD_BIT(view);
      gfx->per_draw_data.multiview_index = view;
      struct kk_draw_command per_view = *data;
      kk_draw_one_view(cmd, &per_view);
   }
   gfx->render.view_mask = view_mask;`,
`   /* Direct multi-draw commands have trailing records beyond draws[0].
    * Indirect records remain in GPU memory, so only their header is copied.
    * Reset the full snapshot for each view: predicate and topology lowering
    * can rewrite its draw representation. */
   size_t size = sizeof(*data);
   if (!data->indirect && data->draw_count > 1u) {
      size_t stride = data->indexed ? sizeof(data->indexed_draws[0])
                                    : sizeof(data->draws[0]);
      size += (size_t)(data->draw_count - 1u) * stride;
   }
   struct kk_draw_command *per_view = ralloc_size(NULL, size);
   if (!per_view) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   /* As in Honeykrisp, replay actual work for each view. A one-bit mask keeps
    * every real query producer at one view per replay; EndQuery publishes the
    * summed first query plus the reset zero entries for the other views. */
   u_foreach_bit(view, view_mask) {
      gfx->render.view_mask = BITFIELD_BIT(view);
      gfx->per_draw_data.multiview_index = view;
      memcpy(per_view, data, size);
      kk_draw_one_view(cmd, per_view);
   }
   gfx->render.view_mask = view_mask;
   ralloc_free(per_view);`));
}
