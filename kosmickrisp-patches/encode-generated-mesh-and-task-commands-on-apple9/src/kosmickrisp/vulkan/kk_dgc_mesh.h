/* SPDX-License-Identifier: MIT */
/* Native Mesh slices reuse the public slot5 and sliced-reader layouts. */
static void
kk_dgc_execute_mesh(struct kk_cmd_buffer *cmd, struct kk_dgc_params p)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_dgc_mesh_config config = {0};
   const bool native = kk_dgc_mesh_prepare(cmd, &p, &config);
   if (cmd->vk.record_result != VK_SUCCESS)
      goto finish;
   if (!native) kk_dgc_set_graphics_indirect(cmd, false);
   kk_dgc_compact_records(&p, false, !native);
   const uint32_t maximum = p.maximum;
   const uint32_t group_limit = config.group_limit;
   uint32_t slices = native ? DIV_ROUND_UP(KK_MESH_MAX_GROUPS, group_limit) : 1;
   const uint32_t capacity = MIN2(maximum, KK_DGC_ICB_SLICE / slices);
   const uint32_t metadata_offset = p.record_stride;
   if (native) {
      uint32_t stride = ALIGN_POT(sizeof(struct kk_mesh_params), 8) +
                        sizeof(struct kk_mesh_batch);
      p.record_stride = ALIGN_POT(p.record_stride + slices * stride, 64);
   }
   p.execution_range = kk_pool_alloc(cmd, 8, 4).gpu;
   p.output = kk_pool_alloc(cmd, (uint64_t)capacity * p.record_stride, 64).gpu;
   if (!p.execution_range || !p.output)
      goto finish;
   struct kk_dgc_mesh_params data = {
      .p = p, .mesh = config.mesh, .metadata_offset = metadata_offset,
      .slices = slices, .group_limit = group_limit,
   };
   memcpy(data.object_local, config.object_local, sizeof(data.object_local));
   memcpy(data.mesh_local, config.mesh_local, sizeof(data.mesh_local));
   void *icb = native ? kk_dgc_new_mesh_icb(dev, capacity * slices, &data) : NULL;
   if (native && !icb) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto finish;
   }
   if (icb) {
      kk_device_add_buffer_to_residency_set(dev, icb);
      util_dynarray_append(&cmd->dgc_commands, icb);
   }
   for (uint32_t first = 0; first < maximum; first += capacity) {
      uint32_t count = MIN2(maximum - first, capacity);
      data.p.first = first;
      data.p.maximum = count;
      struct { struct kk_dgc_mesh_params p; uint64_t id; } upload = {
         data, icb ? kk_dgc_icb_id(icb) : 0,
      };
      uint64_t parameters = kk_pool_upload(cmd, &upload, sizeof(upload), 8).gpu;
      if (!parameters || !kk_dgc_mesh_encode(cmd, cs_get_compute(cmd), parameters,
                                             count, native))
         break;
      if (native) {
         kk_dgc_mesh_execute(cmd, &config, icb, p.execution_range);
      } else {
         for (uint32_t i = 0; i < count; ++i) {
            uint64_t record = p.output + (uint64_t)i * p.record_stride;
            kk_dgc_draw_mesh_sequence(cmd, record, record + p.args_offset);
            if (cmd->vk.record_result != VK_SUCCESS)
               break;
         }
      }
      if (cmd->vk.record_result != VK_SUCCESS)
         break;
   }
finish:
   kk_dgc_set_graphics_indirect(cmd, false);
}
