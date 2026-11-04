/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_LINE_DRAW_H
#define KK_MESH_LINE_DRAW_H

struct kk_mesh_line_state {
   float viewport[16][4];
   float width;
};

static VkResult
kk_mesh_line_consume(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, void *data)
{
   (void)data;
   struct kk_pipeline_handles *pipe;
   struct kk_shader *shader = cmd->state.gfx.shader;
   VkResult result = kk_mesh_get_line_pipeline(kk_cmd_buffer_device(cmd), shader, &pipe);
   if (result != VK_SUCCESS) return result;
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_mesh_line_state state = {.width = kk_polygon_line_width(dyn->rs.line.width)};
   for (unsigned i = 0; i < MIN2(dyn->vp.viewport_count,16u); ++i) {
      const VkViewport *vp = &dyn->vp.viewports[i];
      state.viewport[i][0] = vp->x; state.viewport[i][1] = vp->y;
      state.viewport[i][2] = vp->width; state.viewport[i][3] = vp->height;
   }
   struct kk_ptr config = kk_pool_upload(cmd, &state, sizeof(state), 16);
   if (!config.gpu) return VK_ERROR_OUT_OF_HOST_MEMORY;
   mtl_render_pipeline_state *render = kk_attachment_pipeline(cmd, shader, KK_PIPELINE_MESH_LINE);
   if (!render) return VK_ERROR_INVALID_SHADER_NV;
   mtl_set_cull_mode(batch->encoder,MTL_CULL_MODE_NONE);
   mtl_set_triangle_fill_lines(batch->encoder,false);
   mtl_render_set_pipeline_state(batch->encoder,render);
   for (unsigned base = 0; base < batch->layout.max_primitives; base += 64) {
      const struct kk_mesh_params params = {
         .indirect_count = batch->outputs, .payloads = config.gpu,
         .task_base = base, .predicate_inverted = batch->logical_layer,
      };
      struct kk_ptr p = kk_pool_upload(cmd,&params,sizeof(params),8);
      if (!p.gpu) { result = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
      mtl_set_address(batch->mesh_table,p.gpu,KK_ARGUMENT_MESH_PARAMETERS);
      mtl_render_set_argument_table(batch->encoder,batch->mesh_table,
         MTL_RENDER_STAGE_MESH | MTL_RENDER_STAGE_FRAGMENT);
      mtl_draw_mesh_threadgroups_indirect(batch->encoder,batch->grid,
         (struct mtl_size){1,1,1},(struct mtl_size){1,1,1});
   }
   mtl_set_address(batch->mesh_table,batch->reader_parameters,KK_ARGUMENT_MESH_PARAMETERS);
   mtl_set_cull_mode(batch->encoder,vk_front_face_to_mtl_cull_mode(dyn->rs.cull_mode));
   mtl_set_triangle_fill_lines(batch->encoder,dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE);
   return result;
}

static bool
kk_mesh_line_selected(const struct kk_cmd_buffer *cmd)
{
   return cmd->state.gfx.shader->info.mesh_line &&
      cmd->vk.dynamic_graphics_state.rs.line.mode ==
         VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH;
}
#endif
