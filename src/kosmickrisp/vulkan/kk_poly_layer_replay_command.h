/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_LAYER_REPLAY_COMMAND_H
#define KK_POLY_LAYER_REPLAY_COMMAND_H

static bool
kk_poly_layer_argument_available(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   return dev->poly_layer_argument;
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
   gfx->layer_replay = (struct kk_poly_layer_replay_state){
      .bind = bind, .data = data, .layer_count = layers,
   };
   gfx->poly_replay_active = true;
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   bool point = kk_polygon_expansion_active(cmd);
   if (point != gfx->polygon_points) {
      gfx->polygon_points = point;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
      /* The caller already flushed the selected executable. Rebind fixed
       * raster state only if begin changes the selected native/Poly path. */
      kk_flush_gfx_state(cmd);
   }
   draw->upload_mask |= build_per_draw_upload_mask(cmd);
   return cmd->vk.record_result == VK_SUCCESS;
}

void
kk_poly_layer_replay_end(struct kk_cmd_buffer *cmd)
{
   if (cmd->state.gfx.layer_replay.selector_bound)
      mtl_set_address(cmd->argument_table, 0, KK_ARGUMENT_POLY_LAYER_REPLAY);
   cmd->state.gfx.layer_replay = (struct kk_poly_layer_replay_state){0};
   cmd->state.gfx.poly_replay_active = cmd->state.gfx.poly_retained.consume != NULL;
   if (cmd->state.gfx.polygon_points != kk_polygon_expansion_active(cmd))
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
   struct kk_ptr selectors = {0};
   if (vs->info.poly_layer_replay) {
      assert(kk_poly_layer_argument_available(cmd));
      if (layers > UINT32_MAX / sizeof(struct kk_poly_layer_selector)) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         return;
      }
      selectors = kk_pool_alloc(cmd, layers * sizeof(struct kk_poly_layer_selector), 8u);
      if (unlikely(!selectors.gpu)) return;
      struct kk_poly_layer_selector *values = selectors.cpu;
      for (uint32_t offset = 0; offset < layers; ++offset)
         values[offset] = (struct kk_poly_layer_selector){first + offset, 1};
   }
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
         uint64_t address = selectors.gpu + offset * sizeof(struct kk_poly_layer_selector);
         mtl_set_address(cmd->argument_table, address, KK_ARGUMENT_POLY_LAYER_REPLAY);
         gfx->layer_replay.selector_bound = true;
         gfx->poly_retained.layer_selector_address = address;
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
