/* SPDX-License-Identifier: MIT */
/* Called by the submit-owned path, synchronously joined to a KK worker. */
static void
kk_dgc_execute_set(struct kk_cmd_buffer *cmd, bool preprocessed,
                    const VkGeneratedCommandsInfoEXT *info)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if (!kk_dgc_init(dev)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INITIALIZATION_FAILED);
      return;
   }
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout, info->indirectCommandsLayout);
   VK_FROM_HANDLE(kk_dgc_execution_set, set, info->indirectExecutionSet);
   const bool compute = set->stages & VK_SHADER_STAGE_COMPUTE_BIT;
   struct kk_dgc_program_snapshot *snapshot = kk_dgc_snapshot_programs(cmd, set);
   if (!snapshot) return;
   struct kk_descriptor_state *desc = compute ? &cmd->state.cs.descriptors : &cmd->state.gfx.descriptors;
   const struct kk_descriptor_state saved_desc = *desc;
   struct kk_dgc_program saved = {.shader = {
      cmd->state.shaders[MESA_SHADER_VERTEX] ? &cmd->state.shaders[MESA_SHADER_VERTEX]->vk : NULL,
      cmd->state.shaders[MESA_SHADER_FRAGMENT] ? &cmd->state.shaders[MESA_SHADER_FRAGMENT]->vk : NULL,
      cmd->state.shaders[MESA_SHADER_COMPUTE] ? &cmd->state.shaders[MESA_SHADER_COMPUTE]->vk : NULL,
   }};
   struct vk_shader *saved_graphics[ARRAY_SIZE(kk_dgc_graphics_stages)];
   for (unsigned i = 0; i < ARRAY_SIZE(saved_graphics); ++i) {
      struct kk_shader *shader = cmd->state.shaders[kk_dgc_graphics_stages[i]];
      saved_graphics[i] = shader ? &shader->vk : NULL;
   }
   struct vk_dynamic_graphics_state dynamic = cmd->vk.dynamic_graphics_state;
   struct vk_vertex_input_state vi;
   struct vk_sample_locations_state samples;
   if (dynamic.vi) vi = *dynamic.vi;
   if (dynamic.ms.sample_locations) samples = *dynamic.ms.sample_locations;
   if (!compute) dev->vk.shader_ops->cmd_set_dynamic_graphics_state(&cmd->vk, &set->dynamic);
   kk_dgc_program_bind(cmd, &snapshot->initial, compute);
   if (!preprocessed) kk_dgc_preprocess_inner(kk_cmd_buffer_to_handle(cmd), info, kk_cmd_buffer_to_handle(cmd));
   struct kk_dgc_pipeline_row *rows = vk_zalloc(&dev->vk.alloc,
      set->count * sizeof(*rows), 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!rows) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto restore;
   }
   bool native = true;
   struct kk_dgc_params buffers = {0};
   for (uint32_t i = 0; i < set->count; ++i) {
      const struct kk_dgc_program *program = &snapshot->programs[i];
      if (!program->shader[compute ? 2 : 0]) continue;
      *desc = saved_desc;
      kk_dgc_program_bind(cmd, program, compute);
      bool replay = !compute && (cmd->state.gfx.render.layered_bc || kk_dgc_needs_poly(cmd) ||
         ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED)) &&
          (cmd->state.gfx.index.bytes_per_index == 1 ||
           ((layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB)) && !layout->vk.index_mode_is_dx))));
      native &= !replay;
      if (!compute) {
         kk_dgc_set_graphics_indirect(cmd, !replay);
         kk_dgc_set_linked_table_buffers(cmd, 0, 0);
         kk_dgc_flush_graphics(cmd);
      }
      rows[i].p = kk_dgc_state(cmd, cmd, info);
      if (!rows[i].p.base_root || (!compute && !rows[i].p.base_draw)) goto free_rows;
      rows[i].p.stream = info->preprocessAddress + SNAPSHOT_HEADER;
      rows[i].p.count = info->preprocessAddress + SNAPSHOT_COUNT;
      rows[i].p.stream_stride = layout->token_span;
      rows[i].valid = true;
      if (compute) {
         mtl_compute_pipeline_state *pipeline = kk_dgc_get_compute_pipeline(dev, cmd->state.shaders[MESA_SHADER_COMPUTE]);
         if (!pipeline) { vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INVALID_SHADER_NV); goto free_rows; }
         kk_dgc_pipeline_ids(&rows[i], NULL, pipeline, NULL);
      } else if (!replay) {
         mtl_render_pipeline_state *render = NULL;
         mtl_depth_stencil_state *depth = NULL;
         kk_dgc_selected_graphics(cmd, &render, &depth);
         if (!render) goto free_rows;
         kk_dgc_snapshot_depth(snapshot)[i] = mtl_retain(depth);
         kk_dgc_pipeline_ids(&rows[i], render, NULL, depth);
         struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
         struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
         rows[i].p.layer_replay = dev->poly_layer_argument && vs->info.poly_layer_replay;
         rows[i].p.linked_vs_buffer = cmd->state.gfx.dgc_linked_vs_buffer;
         rows[i].p.linked_fs_buffer = cmd->state.gfx.dgc_linked_fs_buffer;
         rows[i].p.interlock_guard = fs && fs->info.dgc_interlock_guard && dev->interlock_guard ? dev->interlock_guard->gpu : 0;
      }
      /* Capacity covers the union of buffers any selected entry can bind. */
      buffers.linked_vs_buffer |= rows[i].p.linked_vs_buffer;
      buffers.linked_fs_buffer |= rows[i].p.linked_fs_buffer;
      buffers.layer_replay |= rows[i].p.layer_replay;
      buffers.interlock_guard |= rows[i].p.interlock_guard;
   }
   if (cmd->vk.record_result != VK_SUCCESS) goto free_rows;
   struct kk_dgc_params p = {0};
   uint32_t vi_size = 0;
   for (uint32_t i = 0; i < set->count; ++i) {
      if (!rows[i].valid) continue;
      if (native)
         kk_dgc_compact_records(&rows[i].p, compute, false);
      /* Native rows have one draw shape. Reserve the largest layout while
       * retaining each row's immutable-root choice for GPU selection. */
      if (!p.base_root || rows[i].p.record_stride > p.record_stride)
         p = rows[i].p;
      vi_size = MAX2(vi_size, rows[i].p.vertex_input_size);
   }
   if (!native) {
      /* Shared Poly/layer readers retain complete roots and statistics data. */
      p.root_size = ROOT_SIZE;
      p.draw_offset = ALIGN_POT(p.root_size, 8);
      p.args_offset = ALIGN_POT(p.draw_offset + sizeof(struct kk_per_draw_data), 8);
      p.index_offset = ALIGN_POT(p.args_offset + 20, 8);
      p.vertex_input_offset = ALIGN_POT(p.index_offset + 16, 8);
      p.record_stride = ALIGN_POT(p.vertex_input_offset + vi_size, 64);
   }
   uint32_t capacity = native ? MIN2(info->maxSequenceCount, KK_DGC_ICB_SLICE) : 1;
   p.output = kk_pool_alloc(cmd, (uint64_t)capacity * p.record_stride, 64).gpu;
   p.execution_range = native ? kk_pool_alloc(cmd, 8, 4).gpu : 0;
   if (!p.output || (native && !p.execution_range)) goto free_rows;
   for (uint32_t i = 0; i < set->count; ++i) {
      if (!native) rows[i].p.root_size = p.root_size;
      rows[i].p.draw_offset = p.draw_offset;
      rows[i].p.args_offset = p.args_offset;
      rows[i].p.index_offset = p.index_offset;
      rows[i].p.vertex_input_offset = p.vertex_input_offset;
      rows[i].p.record_stride = p.record_stride;
   }
   uint64_t table = kk_pool_upload(cmd, rows, set->count * sizeof(*rows), 8).gpu;
   if (!table) goto free_rows;
   struct kk_dgc_pipeline_state state = { .p = p, .rows = table,
      .pipeline_offset = layout->vk.ies_src_offset_B, .pipeline_count = set->count };
   if (native) {
      void *icb = kk_dgc_new_pipeline_icb(dev, capacity, compute, &buffers);
      if (!icb) { vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY); goto free_rows; }
      kk_device_add_buffer_to_residency_set(dev, icb);
      util_dynarray_append(&cmd->dgc_commands, icb);
      for (uint32_t first = 0; first < info->maxSequenceCount; first += capacity) {
         uint32_t count = MIN2(info->maxSequenceCount - first, capacity);
         state.p.first = first; state.p.maximum = count;
         struct { struct kk_dgc_pipeline_state state; uint64_t id; } data = {state, kk_dgc_icb_id(icb)};
         uint64_t params = kk_pool_upload(cmd, &data, sizeof(data), 8).gpu;
         if (!params || !kk_dgc_pipeline_encode(cmd, cs_get_compute(cmd), params, params + sizeof(state), count, true)) break;
         if (compute) {
            kk_dgc_execute_compute(cs_get_compute(cmd), icb, p.execution_range, NULL);
            KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options,
               cmd->metal.compute, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
         } else {
            kk_dgc_set_graphics_indirect(cmd, true);
            kk_dgc_flush_graphics(cmd);
            kk_dgc_execute_graphics(cs_get_render(cmd), icb, p.execution_range);
         }
      }
   } else {
      kk_dgc_set_graphics_indirect(cmd, false);
      struct kk_ptr sequence = kk_pool_alloc(cmd, (uint64_t)info->maxSequenceCount * 4, 4);
      struct kk_ptr states = kk_pool_alloc(cmd, set->count * sizeof(state), 8);
      if (!sequence.gpu || !states.gpu) goto free_rows;
      for (uint32_t i = 0; i < info->maxSequenceCount; ++i) ((uint32_t *)sequence.cpu)[i] = i;
      for (uint32_t i = 0; i < set->count; ++i) {
         state.target = i;
         ((struct kk_dgc_pipeline_state *)states.cpu)[i] = state;
      }
      for (uint32_t first = 0; first < info->maxSequenceCount; ++first) {
         for (uint32_t i = 0; i < set->count; ++i) {
            if (!rows[i].valid) continue;
            *desc = saved_desc;
            kk_dgc_program_bind(cmd, &snapshot->programs[i], false);
            if (!kk_dgc_pipeline_encode(cmd, cs_get_compute(cmd), states.gpu + i * sizeof(state), sequence.gpu + first * 4, 1, false)) goto free_rows;
            if (cmd->state.gfx.render.layered_bc)
               kk_dgc_execute_layered_sequence(cmd, p.output, p.output + p.args_offset, p.output + p.index_offset, p.action == 1, 0);
            else kk_dgc_draw_sequence(cmd, p.output, p.output + p.args_offset, p.output + p.index_offset, p.action == 1, 0);
            if (cmd->vk.record_result != VK_SUCCESS) goto free_rows;
         }
      }
   }
free_rows:
   vk_free(&dev->vk.alloc, rows);
restore:
   *desc = saved_desc;
   if (compute) kk_dgc_program_bind(cmd, &saved, true);
   else dev->vk.shader_ops->cmd_bind_shaders(&cmd->vk, ARRAY_SIZE(kk_dgc_graphics_stages),
      kk_dgc_graphics_stages, saved_graphics);
   if (!compute) {
      cmd->vk.dynamic_graphics_state = dynamic;
      if (dynamic.vi) *dynamic.vi = vi;
      if (dynamic.ms.sample_locations) *dynamic.ms.sample_locations = samples;
      vk_dynamic_graphics_state_dirty_all(&cmd->vk.dynamic_graphics_state);
      kk_dgc_set_graphics_indirect(cmd, false);
   }
   desc->root_dirty = true;
   if (cmd->metal.compute) kk_cmd_bind_compute_arguments(cmd, cmd->metal.compute, cmd->argument_table);
   kk_cmd_bind_root_to_argument_table(cmd, cmd->state.root_addr);
}
