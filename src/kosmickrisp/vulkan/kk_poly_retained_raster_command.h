/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_RETAINED_RASTER_COMMAND_H
#define KK_POLY_RETAINED_RASTER_COMMAND_H

struct kk_poly_retained_lifetime {
   mtl_render_encoder *encoder;
   bool active;
};

VkResult
kk_poly_retained_begin(struct kk_cmd_buffer *cmd, struct kk_draw_command *draw,
   kk_poly_retained_consume consume, void *data)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   if (!draw || !consume || gfx->poly_retained.consume || !gfx->shader ||
       !gfx->shader->info.poly_layer_replay ||
       gfx->shader->info.stage == MESA_SHADER_MESH)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   gfx->poly_retained = (struct kk_poly_retained_state){.consume = consume, .data = data};
   /* The captured sibling and original native pair were compiled together.
    * Select the former before uploads and producer preparation, once. */
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   if (!gfx->polygon_points) {
      gfx->polygon_points = true;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE);
   }
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
   kk_flush_gfx_state(cmd);
   draw->upload_mask |= build_per_draw_upload_mask(cmd);
   return cmd->vk.record_result;
}

void
kk_poly_retained_end(struct kk_cmd_buffer *cmd)
{
   cmd->state.gfx.poly_retained = (struct kk_poly_retained_state){0};
   BITSET_SET(cmd->vk.dynamic_graphics_state.dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
}

VkResult
kk_poly_retained_prepare_range(const struct kk_poly_retained_raster *r,
   uint64_t range_address, uint64_t *result)
{
   if (!r || !range_address || !result || !r->lifetime->active ||
       r->lifetime->encoder) return VK_ERROR_UNKNOWN;
   struct kk_ptr out = kk_pool_alloc(r->command, 4u * sizeof(uint32_t), 4u);
   if (!out.gpu) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   libkk_polygon_retained_draw(r->command, kk_grid_1d(1), true,
      r->geometry_params, range_address, out.gpu, r->conservative);
   *result = out.gpu;
   return r->command->vk.record_result;
}

VkResult
kk_poly_retained_draw(const struct kk_poly_retained_raster *r,
   mtl_render_encoder *encoder, mtl_argument_table *fragment, uint64_t indirect_draw)
{
   if (!r || !encoder || !fragment || !indirect_draw ||
       !r->lifetime->active ||
       (r->lifetime->encoder && r->lifetime->encoder != encoder) ||
       r->command->metal.render != encoder ||
       r->command->state.gfx.shader != r->producer)
      return VK_ERROR_UNKNOWN;
   r->lifetime->encoder = encoder;
   /* Vertex parameters keep the original draw/Layer convention; fragment
    * phase context can never replace them. No encoder transition here. */
   mtl_render_set_argument_table(encoder, r->vertex_table, MTL_RENDER_STAGE_VERTEX);
   mtl_render_set_argument_table(encoder, fragment, MTL_RENDER_STAGE_FRAGMENT);
   mtl_draw_primitives_indirect(encoder, r->primitive_type, indirect_draw);
   return VK_SUCCESS;
}

static void
kk_poly_retained_dispatch_draw(struct kk_cmd_buffer *cmd, struct kk_draw_data draw)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   if (!gfx->poly_retained.consume) {
      kk_dispatch_draw(cmd, draw);
      return;
   }
   /* Empty/discarded draws have no captured raster work. */
   if (!kk_grid_is_indirect(draw.grid) && !draw.grid.size.x) return;
   if (!gfx->polygon_points || !kk_grid_is_indirect(draw.grid) ||
       draw.index.el_size_B || !gfx->polygon_capture) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_UNKNOWN);
      return;
   }
   cmd->writable_msaa_render_writes |= cmd->writable_msaa_draw_writes;
   struct kk_poly_retained_storage {
      struct kk_poly_retained_raster raster;
      struct kk_poly_retained_lifetime lifetime;
   };
   struct kk_ptr storage = kk_pool_alloc(cmd, sizeof(struct kk_poly_retained_storage), 8u);
   if (!storage.gpu) return;
   struct kk_poly_retained_storage *record = storage.cpu;
   record->lifetime = (struct kk_poly_retained_lifetime){.active = true};
   struct kk_poly_retained_raster *r = &record->raster;
   *r = (struct kk_poly_retained_raster){
      .command = cmd, .producer = gfx->shader, .vertex_table = cmd->argument_table,
      .lifetime = &record->lifetime,
      .descriptor = kk_poly_retained_pipeline_descriptor(gfx->shader, gfx->conservative),
      .geometry_params = gfx->per_draw_data.geometry_params,
      .capture = gfx->polygon_capture,
      .slot_count_address = gfx->polygon_capture + offsetof(struct poly_geometry_capture, clip_grid),
      .complete_draw = draw.grid.addr,
      .per_draw_address = gfx->poly_retained.per_draw_address,
      .layer_selector_address = gfx->poly_retained.layer_selector_address,
      .primitive_type = draw.primitive_type, .view_mask = gfx->render.view_mask,
      .topology = draw.primitive_type == MTL_PRIMITIVE_TYPE_POINT ?
         MTL_PRIMITIVE_TOPOLOGY_CLASS_POINT :
         draw.primitive_type == MTL_PRIMITIVE_TYPE_LINE ?
         MTL_PRIMITIVE_TOPOLOGY_CLASS_LINE : MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE,
      .logical_layer = gfx->poly_retained.logical_layer,
      .conservative = gfx->conservative,
   };
   VkResult result = r->descriptor ? gfx->poly_retained.consume(cmd, gfx->poly_retained.data, r) :
      VK_ERROR_UNKNOWN;
   record->lifetime.active = false;
   if (result != VK_SUCCESS) vk_command_buffer_set_error(&cmd->vk, result);
}
#endif
