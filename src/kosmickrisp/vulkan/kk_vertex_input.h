/* SPDX-License-Identifier: MIT */
/* Shared static/dynamic vertex preparation. Included after the VBO clamp helper. */
static bool
kk_prepare_vertex_input(struct kk_cmd_buffer *cmd,
   const struct kk_graphics_state *gfx,
   const struct vk_dynamic_graphics_state *dyn, const struct kk_shader *vs,
   struct kk_root_descriptor_table *root)
{
   struct kk_vertex_fetch input[KK_MAX_ATTRIBS];
   unsigned slot = 0;
   u_foreach_bit(i, vs->info.vs.attribs_read) {
      if (dyn->vi->attributes_valid & BITFIELD_BIT(i)) {
         struct vk_vertex_attribute_state attr = dyn->vi->attributes[i];
         struct kk_addr_range vb = gfx->vb.addr_range[attr.binding];
         enum pipe_format format = vk_format_to_pipe_format(attr.format);
         uint32_t stride = dyn->vi_binding_strides[attr.binding];
         root->draw.attrib_clamps[slot] = kk_calculate_vbo_clamp(
            vb.addr, 0, format, vb.range, stride, attr.offset,
            &root->draw.attrib_base[slot]);
         root->draw.buffer_strides[attr.binding] = stride;
         if (vs->info.vs.dynamic_vi) {
            const struct vk_vertex_binding_state *binding =
               &dyn->vi->bindings[attr.binding];
            input[slot].format = kk_vbo_encode_format(format);
            if (binding->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE)
               input[slot].format |= KK_VBO_FMT_INSTANCED;
            input[slot].divisor = binding->divisor;
            input[slot].stride = stride;
         }
      } else {
         if (vs->info.vs.dynamic_vi)
            input[slot] = (struct kk_vertex_fetch){0};
         root->draw.attrib_clamps[slot] = 0;
         root->draw.attrib_base[slot] = 0;
      }
      slot++;
   }
   if (vs->info.vs.dynamic_vi && slot) {
      struct kk_ptr upload = kk_pool_upload(cmd, input, slot * sizeof(input[0]), 4);
      if (!upload.gpu)
         return false;
      root->draw.vertex_input = upload.gpu;
   }

   return true;
}

static bool kk_flush_vertex_input(struct kk_cmd_buffer *cmd)
{
   bool success = kk_prepare_vertex_input(cmd, &cmd->state.gfx,
      &cmd->vk.dynamic_graphics_state, cmd->state.gfx.shader,
      &cmd->state.gfx.descriptors.root);
   cmd->state.gfx.descriptors.root_dirty = true;
   return success;
}
