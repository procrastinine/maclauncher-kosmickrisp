/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_LAYER_REPLAY_COMMAND_H
#define KK_POLY_LAYER_REPLAY_COMMAND_H

static bool
kk_poly_layer_argument_available(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   return dev->vk.enabled_features.shaderOutputLayer ||
          dev->vk.enabled_features.geometryShader ||
          (dev->vk.enabled_features.multiview || dev->vk.enabled_features.pipelineFragmentShadingRate ||
          dev->vk.enabled_features.primitiveFragmentShadingRate || dev->vk.enabled_features.attachmentFragmentShadingRate);
}

bool
kk_poly_layer_replay_begin(struct kk_cmd_buffer *cmd, struct kk_draw_command *draw,
                          uint32_t layers,
                          kk_poly_layer_bind bind, void *data)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   /* The outer Poly loop supplies one actual view at a time. Captured
    * stages and their original queries execute once for that view, before
    * any physical-slice raster replay. Mesh owns its separate entry. */
   if (!draw || !layers || !bind || gfx->layer_replay.bind ||
       (gfx->render.view_mask & (gfx->render.view_mask - 1u)) ||
       !gfx->shader || gfx->shader->info.stage == MESA_SHADER_MESH)
      return false;
   if (kk_poly_layer_argument_available(cmd))
      mtl_set_address(cmd->argument_table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
   gfx->layer_replay = (struct kk_poly_layer_replay_state){
      .bind = bind, .data = data, .layer_count = layers,
   };
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   bool point = kk_polygon_expansion_active(cmd);
   if (point != gfx->polygon_points) {
      gfx->polygon_points = point;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE);
   }
   BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
   /* Resolve only fixed raster state. Original stages still run below the
    * caller's one existing preparation boundary. */
   kk_flush_gfx_state(cmd);
   draw->upload_mask |= build_per_draw_upload_mask(cmd);
   return true;
}

void
kk_poly_layer_replay_end(struct kk_cmd_buffer *cmd)
{
   if (kk_poly_layer_argument_available(cmd))
      mtl_set_address(cmd->argument_table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
   cmd->state.gfx.layer_replay = (struct kk_poly_layer_replay_state){0};
   BITSET_SET(cmd->vk.dynamic_graphics_state.dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
}

/* Called after predicates, original stages, XFB, clipping and pre-raster
 * queries prepare the draw once. Binding must not reset the command heap,
 * issue a Vulkan draw or replace the linked graphics pipeline. */
static void
kk_poly_layer_dispatch_draw(struct kk_cmd_buffer *cmd, struct kk_draw_data draw)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   const struct kk_poly_layer_replay_state replay = gfx->layer_replay;
   if (!replay.bind) {
      kk_poly_retained_dispatch_draw(cmd, draw);
      return;
   }
   const struct kk_shader *vs = gfx->shader;
   /* No Layer output means logical layer zero, once. Clears still cover all
    * requested attachment layers through the ordinary render path. */
   uint32_t view_mask = gfx->render.view_mask;
   assert(!(view_mask & (view_mask - 1u)));
   uint32_t first = view_mask ? ffs(view_mask) - 1u : 0u;
   uint32_t layers = view_mask ? 1u :
      (vs->info.poly_layer_replay ? replay.layer_count : 1u);
   for (uint32_t offset = 0; offset < layers; ++offset) {
      uint32_t layer = first + offset;
      VkResult result = replay.bind(cmd, replay.data, layer);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         return;
      }
      assert(gfx->shader == vs);
      kk_flush_gfx_state(cmd);
      if (replay.per_draw_address)
         mtl_set_address(cmd->argument_table, replay.per_draw_address,
                         KK_ARGUMENT_PER_DRAW);
      if (vs->info.poly_layer_replay) {
         assert(kk_poly_layer_argument_available(cmd));
         const struct kk_poly_layer_selector selector = {layer, 1};
         struct kk_ptr data = kk_pool_upload(cmd, &selector, sizeof(selector), 8u);
         if (unlikely(!data.gpu)) return;
         mtl_set_address(cmd->argument_table, data.gpu, KK_ARGUMENT_POLY_LAYER_REPLAY);
         gfx->poly_retained.layer_selector_address = data.gpu;
         gfx->poly_retained.logical_layer = layer;
      }
      /* Attachment aliases have physical index zero. Never amplify the
       * cached draw again after the original view has been produced. */
      if (view_mask) {
         uint32_t physical_layer = 0;
         mtl_set_vertex_amplification_count(cs_get_render(cmd), &physical_layer, 1);
      }
      kk_poly_retained_dispatch_draw(cmd, draw);
      if (cmd->vk.record_result != VK_SUCCESS) return;
   }
}
#endif
