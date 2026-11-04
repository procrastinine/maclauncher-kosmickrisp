/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_VIRTUAL_EXACT_H
#define KK_MESH_VIRTUAL_EXACT_H
/* The continuation owns one real Task batch at a time. Capacity is bounded
 * metadata, never a count of native draws to encode on the CPU. */
static void
kk_draw_mesh_virtual_exact(struct kk_cmd_buffer *cmd, uint64_t grid, uint64_t count)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_shader *shader = cmd->state.gfx.shader;
   const struct kk_mesh_shader_info *info = &shader->mesh->info;
   struct kk_pipeline_handles *pipe;
   VkResult result = kk_mesh_get_sliced_pipeline(dev, shader, true, &pipe);
   if (result != VK_SUCCESS) { vk_command_buffer_set_error(&cmd->vk, result); return; }
   kk_flush_gfx_state(cmd);
   mtl_argument_table *table = kk_mesh_table(cmd);
   if (!table || !kk_mesh_bind_draw(cmd, table, 0) || !kk_mesh_scratch(cmd)) return;
   struct kk_mesh_exact_batch *batch = &cmd->mesh_chunk.exact;
   unsigned native_limit = kk_dgc_native_mesh_groups(kk_device_physical(dev)->info.gpu_apple_family);
   bool native = native_limit && !info->flat_heap;
   batch->span = MIN2(kk_mesh_canonical_span(cmd), native ? native_limit : KK_MESH_MAX_PHYSICAL_GROUPS);
   uint64_t maximum = (uint64_t)cmd->mesh_chunk.groups * MAX2(info->task_grid_bound, 1u);
   batch->capacity = native ? DIV_ROUND_UP(maximum, batch->span) : 32u;
   struct kk_ptr params = kk_pool_alloc(cmd, batch->capacity * sizeof(struct kk_mesh_params), 8);
   struct kk_ptr grids = kk_pool_alloc(cmd, batch->capacity * 3u * sizeof(uint32_t), 4);
   struct kk_ptr range = kk_pool_alloc(cmd, 8, 4);
   if (!params.gpu || !grids.gpu || !range.gpu) return;
   batch->parameters = params.gpu; batch->host_parameters = params.cpu;
   batch->grids = grids.gpu; batch->host_grids = grids.cpu; batch->range = range.gpu;
   struct kk_mesh_params p = kk_mesh_parameters(cmd, grid, count, 0, 1);
   p.payloads = cmd->state.gfx.mesh_payloads;
   p.records = cmd->state.gfx.mesh_records;
   p.batch = cmd->state.gfx.mesh_batch;
   p.payload_stride = kk_mesh_payload_stride(info);
   /* prepare uses pad as a nonzero marker for the exact group limit. */
   p.pad = batch->span;
   p.max_draw_count = cmd->mesh_chunk.groups;
   batch->mesh = p;
   for (uint32_t i = 0; i < batch->capacity; ++i) {
      batch->host_parameters[i] = p;
      batch->host_parameters[i].pad = i * batch->span;
   }
   uint64_t prepare = kk_pool_upload(cmd, &p, sizeof(p), 8).gpu;
   if (!prepare) return;
   mtl_render_pipeline_state *render = NULL;
   if (native) {
      kk_dgc_set_graphics_indirect(cmd, true);
      kk_flush_gfx_state(cmd);
      render = kk_attachment_pipeline(cmd, shader, KK_PIPELINE_MESH_VIRTUAL_SLICED);
      if (!render) return;
      kk_dgc_set_linked_table_buffers(cmd, 0, 0);
      const struct kk_mesh_fragment_program *fragment = kk_mesh_fragment(shader);
      if (fragment && fragment->bind_arguments)
         fragment->bind_arguments(fragment, shader, pipe, KK_PIPELINE_MESH_VIRTUAL_SLICED, render, table);
   }
   mtl_compute_encoder *enc = cs_get_compute(cmd);
   kk_cmd_bind_compute_arguments(cmd, enc, cmd->argument_table);
   libkk_mesh_prepare(cmd, kk_grid_1d(1), true, prepare);
   mtl_set_address(table, cmd->state.gfx.descriptors.root.addr, KK_ARGUMENT_ROOT);
   mtl_set_address(table, prepare, KK_ARGUMENT_MESH_PARAMETERS);
   kk_cmd_bind_compute_arguments(cmd, enc, table);
   mtl_compute_pipeline_state *task = kk_cmd_sampler_compute_pipeline(cmd, shader, pipe->gfx.pre_render[0]);
   if (!task) return;
   mtl_compute_set_pipeline_state(enc, task);
   mtl_dispatch_threadgroups_with_indirect_buffer(enc,
      p.batch + offsetof(struct kk_mesh_batch, task_dispatch), info->task_local_size);
   kk_cmd_bind_compute_arguments(cmd, enc, cmd->argument_table);
   if (native) {
      struct kk_mesh_expand_params expansion = {
         .mesh = p, .root = cmd->state.gfx.descriptors.root.addr,
         .samplers = dev->samplers.table.bo->gpu,
         .per_draw = cmd->state.gfx.mesh_per_draw_address,
         .linked_vs = cmd->state.gfx.dgc_linked_vs_buffer,
         .linked_fs = cmd->state.gfx.dgc_linked_fs_buffer,
         .guard = shader->info.dgc_interlock_guard && dev->interlock_guard ? dev->interlock_guard->gpu : 0,
         .parameters = params.gpu, .grids = grids.gpu, .range = range.gpu,
         .limit = batch->span, .capacity = batch->capacity,
         .local = {info->local_size.x, info->local_size.y, info->local_size.z},
      };
      if (!kk_mesh_expand_native(cmd, &expansion, &batch->icb)) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INVALID_SHADER_NV); return;
      }
      kk_flush_gfx_state(cmd);
      if (cmd->vk.record_result != VK_SUCCESS) return;
      mtl_render_encoder *render_enc = cs_get_render(cmd);
      mtl_render_set_pipeline_state(render_enc, render);
      kk_dgc_execute_graphics(render_enc, batch->icb, batch->range);
      kk_dgc_set_graphics_indirect(cmd, false);
      cmd->state.dirty_shaders |= BITFIELD_BIT(MESA_SHADER_MESH);
   } else {
      libkk_mesh_prefix_expand(cmd, kk_grid_1d(1), true, prepare,
         batch->grids, batch->range, batch->span, batch->capacity);
      /* Apple7/8 records one real-batch draw. Zero output is a GPU-empty
       * grid; only a published overflow total creates more native draws. */
      kk_mesh_render_slices(cmd, pipe, table, batch->grids, batch->parameters, 1, true,
         cmd->mesh_chunk.draw, cmd->mesh_chunk.base, batch->span, maximum);
   }
}
#endif
