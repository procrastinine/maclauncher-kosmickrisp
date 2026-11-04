/* SPDX-License-Identifier: MIT */
/* DGC owns this adapter; Mesh owns selection, slot5 and the synchronous draw
 * and retained-reader lifetimes. Never select the executable's raw PSO. */
bool
kk_dgc_mesh_prepare(struct kk_cmd_buffer *cmd, struct kk_dgc_params *p,
                    struct kk_dgc_mesh_config *config)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_shader *s = cmd->state.gfx.shader;
   const struct kk_mesh_shader_info *info = &s->mesh->info;
   if (s->info.dgc_replay || s->info.depth_bounds_emulation || info->flat_heap ||
       info->dynamic_provoking ||
       kk_mesh_retained_active(cmd) || cmd->state.gfx.render.layered_bc ||
       cmd->state.gfx.render.view_mask || s->pipeline.gfx.pre_render_count)
      return false;
   uint32_t span = kk_dgc_mesh_group_limit(dev);
   if (!span) return false;
   if (info->has_task)
      span /= MAX2(info->task_grid_bound, 1u);
   if (span >= KK_MESH_MAX_DIMENSION)
      span = (span / KK_MESH_MAX_DIMENSION) * KK_MESH_MAX_DIMENSION;
   if (!span) return false;
   /* The sliced native wrapper restores original task/mesh group coordinates
    * and clips padded rows before entering any application stage. */
   struct kk_pipeline_handles *pipe;
   VkResult result = kk_mesh_get_sliced_pipeline(dev, s, false, &pipe);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return false;
   }
   kk_dgc_set_graphics_indirect(cmd, true);
   kk_flush_gfx_state(cmd);
   mtl_render_pipeline_state *render = kk_attachment_pipeline(cmd, s, KK_PIPELINE_MESH_SLICED);
   if (!render) return false;
   mtl_argument_table *table = kk_mesh_table(cmd);
   if (!table) return false;
   kk_dgc_set_linked_table_buffers(cmd, 0, 0);
   const struct kk_mesh_fragment_program *fragment = kk_mesh_fragment(s);
   if (fragment && fragment->bind_arguments)
      fragment->bind_arguments(fragment, s, pipe, KK_PIPELINE_MESH_SLICED, render, table);
   /* Snapshot after the exact attachment/sampler/ICB variant was selected. */
   p->linked_vs_buffer = cmd->state.gfx.dgc_linked_vs_buffer;
   p->linked_fs_buffer = cmd->state.gfx.dgc_linked_fs_buffer;
   p->interlock_guard = s->info.dgc_interlock_guard && dev->interlock_guard
      ? dev->interlock_guard->gpu : 0;
   config->pipeline = render;
   config->group_limit = span;
   config->mesh = kk_mesh_parameters(cmd, 0, 0, 0, 1);
   struct mtl_size object = info->has_task ? info->task_local_size : (struct mtl_size){1,1,1};
   config->object_local[0] = object.x;
   config->object_local[1] = object.y;
   config->object_local[2] = object.z;
   config->mesh_local[0] = info->local_size.x;
   config->mesh_local[1] = info->local_size.y;
   config->mesh_local[2] = info->local_size.z;
   return true;
}

void
kk_dgc_mesh_execute(struct kk_cmd_buffer *cmd, const struct kk_dgc_mesh_config *config,
                    void *icb, uint64_t range)
{
   kk_flush_gfx_state(cmd);
   if (cmd->vk.record_result != VK_SUCCESS) return;
   mtl_render_encoder *encoder = cs_get_render(cmd);
   mtl_render_set_pipeline_state(encoder, config->pipeline);
   kk_dgc_execute_graphics(encoder, icb, range);
   /* The ordinary flush must restore its own selected main PSO next time. */
   cmd->state.dirty_shaders |= BITFIELD_BIT(MESA_SHADER_MESH);
}

void
kk_dgc_draw_mesh_sequence(struct kk_cmd_buffer *cmd, uint64_t root, uint64_t args)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   uint64_t saved_root = gfx->generated_root;
   uint64_t saved_address = gfx->descriptors.root.addr;
   gfx->generated_root = root;
   gfx->descriptors.root_dirty = true;
   /* This route calls the Mesh draw directly, not through the dispatch
    * table, so it begins the software depth-bounds transaction itself. */
   int bounds = kk_depth_bounds_mesh_begin(kk_cmd_buffer_to_handle(cmd));
   if (bounds >= 0) {
      kk_draw_mesh(cmd, (struct mtl_size){0}, args, 0, 1, 12, false);
      if (bounds) kk_mesh_retained_transaction_end(cmd);
   }
   gfx->generated_root = saved_root;
   gfx->descriptors.root.addr = saved_address;
   gfx->descriptors.root_dirty = true;
   /* All producer/retained readers precede reuse of generated scratch. */
   cs_end(cmd);
}
