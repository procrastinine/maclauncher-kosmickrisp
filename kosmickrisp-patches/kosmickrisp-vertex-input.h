/* SPDX-License-Identifier: MIT */
/* Shared static/dynamic vertex preparation. Included after the VBO clamp helper. */
static bool
kk_flush_vertex_input(struct kk_cmd_buffer *cmd)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_descriptor_state *desc = &gfx->descriptors;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   struct kk_vertex_fetch input[KK_MAX_ATTRIBS];
   unsigned slot = 0;
   u_foreach_bit(i, vs->info.vs.attribs_read) {
      if (dyn->vi->attributes_valid & BITFIELD_BIT(i)) {
         struct vk_vertex_attribute_state attr = dyn->vi->attributes[i];
         struct kk_addr_range vb = gfx->vb.addr_range[attr.binding];
         enum pipe_format format = vk_format_to_pipe_format(attr.format);
         uint32_t stride = dyn->vi_binding_strides[attr.binding];
         desc->root.draw.attrib_clamps[slot] = kk_calculate_vbo_clamp(
            vb.addr, 0, format, vb.range, stride, attr.offset,
            &desc->root.draw.attrib_base[slot]);
         desc->root.draw.buffer_strides[attr.binding] = stride;
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
         desc->root.draw.attrib_clamps[slot] = 0;
         desc->root.draw.attrib_base[slot] = 0;
      }
      slot++;
   }
   if (vs->info.vs.dynamic_vi && slot) {
      struct kk_ptr upload = kk_pool_upload(cmd, input, slot * sizeof(input[0]), 4);
      if (!upload.gpu)
         return false;
      desc->root.draw.vertex_input = upload.gpu;
   }

   desc->root_dirty = true;
   return true;
}
