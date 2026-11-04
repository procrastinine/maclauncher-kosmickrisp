/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_PREPARED_H
#define KK_MESH_PREPARED_H
static void kk_mesh_pass_draw(struct kk_cmd_buffer *, struct mtl_size, uint64_t, uint64_t,
   uint32_t, uint32_t, bool);

/* Completed pass preparation provides original grids. Materialize native
 * slice arguments on the CPU before the render encoder consumes them. */
static void
kk_mesh_prepared_slices(struct kk_cmd_buffer *cmd, struct mtl_size direct)
{
   struct kk_shader *shader = cmd->state.gfx.shader;
   struct kk_pipeline_handles *pipe;
   VkResult result = kk_mesh_get_sliced_pipeline(kk_cmd_buffer_device(cmd), shader, false, &pipe);
   if (result != VK_SUCCESS) { vk_command_buffer_set_error(&cmd->vk, result); return; }
   kk_flush_gfx_state(cmd);
   mtl_argument_table *table = kk_mesh_table(cmd);
   if (!table || !kk_mesh_bind_draw(cmd, table, cmd->mesh_prepared_draw)) return;
   const struct kk_mesh_shader_info *info = &shader->mesh->info;
   uint32_t span = KK_MESH_MAX_PHYSICAL_GROUPS / MAX2(info->has_task ? info->task_grid_bound : 1u, 1u);
   if (span > KK_MESH_MAX_DIMENSION) span -= span % KK_MESH_MAX_DIMENSION;
   uint32_t total = (uint64_t)direct.x * direct.y * direct.z;
   uint32_t slices = DIV_ROUND_UP(total, span);
   struct kk_ptr batch = kk_pool_alloc(cmd, sizeof(struct kk_mesh_batch), 8);
   struct kk_ptr params = kk_pool_alloc(cmd, (uint64_t)slices * sizeof(struct kk_mesh_params), 8);
   struct kk_ptr grids = kk_pool_alloc(cmd, (uint64_t)slices * 3u * sizeof(uint32_t), 4);
   if (!batch.gpu || !params.gpu || !grids.gpu) return;
   struct kk_mesh_batch *b = batch.cpu;
   memset(b, 0, sizeof(*b));
   b->input_grid[0] = direct.x; b->input_grid[1] = direct.y; b->input_grid[2] = direct.z;
   struct kk_mesh_params p = kk_mesh_parameters(cmd, 0, 0, cmd->mesh_prepared_draw, 1);
   p.predicate = 0; p.batch = batch.gpu; p.payload_stride = span;
   for (uint32_t i = 0; i < slices; ++i) {
      ((struct kk_mesh_params *)params.cpu)[i] = p;
      ((struct kk_mesh_params *)params.cpu)[i].task_base = i * span;
      kk_mesh_slice_grid((uint32_t *)grids.cpu + i * 3u, MIN2(total - i * span, span));
   }
   if (!kk_mesh_begin_batch(cmd, slices, false)) return;
   kk_mesh_render_slices_native(cmd, pipe, table, grids.gpu, params.gpu, slices, false);
}
#endif
