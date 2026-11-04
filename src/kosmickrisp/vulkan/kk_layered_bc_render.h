/* SPDX-License-Identifier: MIT */
/* Included after the common attachment descriptor helpers in kk_cmd_draw.c. */
#ifndef KK_LAYERED_BC_RENDER_H
#define KK_LAYERED_BC_RENDER_H

static void
kk_layered_bc_refresh_attachments(struct kk_cmd_buffer *cmd, bool force_load)
{
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   mtl_render_pass_descriptor *descriptor = cmd->state.gfx.render_pass_descriptor;
   kk_set_color_attachments(cmd, descriptor, render, &cmd->vk.dynamic_graphics_state,
                            force_load, render->rendering_to_whole_framebuffer, true);
   const struct kk_attachment *attachments[] = {&render->depth_att,
                                               &render->stencil_att};
   for (unsigned i = 0; i < ARRAY_SIZE(attachments); ++i) {
      const struct kk_attachment *attachment = attachments[i];
      if (!attachment->iview) continue;
      mtl_render_pass_attachment_descriptor *at = i == 0 ?
         kk_render_pass_depth(cmd, descriptor) :
         kk_render_pass_stencil(cmd, descriptor);
      bool resolve = kk_attachment_do_renderpass_resolve(attachment,
         render->rendering_to_whole_framebuffer,
         i == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT);
      kk_fill_common_attachment_description(at, attachment, render, force_load,
                                            resolve);
   }
   mtl_render_pass_descriptor_set_render_target_array_length(descriptor, 1);
}

VkResult
kk_layered_bc_bind_layer(struct kk_cmd_buffer *cmd, void *data, uint32_t layer)
{
   struct kk_rendering_state *render = data;
   assert(render == &cmd->state.gfx.render && render->layered_bc);
   const uint32_t views = render->layered_bc_view_mask;
   if (views ? (layer >= 32 || !(views & BITFIELD_BIT(layer))) :
               layer >= render->layer_count)
      return VK_ERROR_UNKNOWN;
   if (layer == render->layered_bc_layer)
      return VK_SUCCESS;
   if (cmd->metal.render)
      kk_apply_attachment_store_ops(cmd, true);
   cs_end(cmd);
   render->layered_bc_layer = layer;
   kk_layered_bc_refresh_attachments(cmd, true);
   cmd->state.gfx.need_to_start_render_pass = true;
   kk_cmd_buffer_dirty_all_gfx(cmd);
   return VK_SUCCESS;
}

static bool
kk_layered_bc_attachment_needs_initial_pass(const struct kk_attachment *att)
{
   return att->iview && (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR ||
                        att->resolve_mode != VK_RESOLVE_MODE_NONE);
}

/* Execute the original load/clear once for every requested logical layer,
 * even when no user draw writes Layer. Re-entry subsequently always loads.
 * The real common store/resolve path preserves all paired attachments. */
static bool
kk_layered_bc_start_rendering(struct kk_cmd_buffer *cmd, bool force_load)
{
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   if (!render->layered_bc) {
      cs_start_render(cmd);
      return true;
   }
   /* Multiview layerCount may be1. Actual retained view bits determine
    * which physical slices are initialized; draw replay remains Poly-owned. */
   const uint32_t views = render->layered_bc_view_mask;
   const uint32_t limit = views ? util_last_bit(views) : render->layer_count;
   bool initialize = kk_layered_bc_attachment_needs_initial_pass(&render->depth_att) ||
                     kk_layered_bc_attachment_needs_initial_pass(&render->stencil_att);
   for (uint32_t i = 0; i < render->color_att_count; ++i)
      initialize |= kk_layered_bc_attachment_needs_initial_pass(&render->color_att[i]);
   /* LOAD/NONE do no work without a draw; DONT_CARE may retain its old bits.
    * No empty native pass/store/barrier is needed on untouched layers.
    * Clears and resolves still visit every requested logical layer. */
   if (initialize) {
      for (uint32_t layer = 0; layer < limit; ++layer) {
         if (views && !(views & BITFIELD_BIT(layer)))
            continue;
         render->layered_bc_layer = layer;
         kk_layered_bc_refresh_attachments(cmd, force_load);
         cmd->state.gfx.need_to_start_render_pass = false;
         cs_start_render(cmd);
         kk_apply_attachment_store_ops(cmd, true);
         cs_end(cmd);
      }
   }
   render->layered_bc_layer = views ? ffs(views) - 1 : 0;
   kk_layered_bc_refresh_attachments(cmd, true);
   return true;
}

static bool
kk_layered_bc_prepare_draw(struct kk_cmd_buffer *cmd)
{
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   if (!render->layered_bc)
      return true;
   const uint32_t layer = render->view_mask ? ffs(render->view_mask) - 1 : 0;
   VkResult result = kk_layered_bc_bind_layer(cmd, render, layer);
   if (result != VK_SUCCESS)
      vk_command_buffer_set_error(&cmd->vk, result);
   return result == VK_SUCCESS;
}

/* The original draw executes producer stages once. Poly supplies pair
 * selection, immutable arg7 upload and cached-output raster replay. */
static bool
kk_layered_bc_begin_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *draw)
{
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   if (!render->layered_bc)
      return true;
   if (kk_poly_layer_replay_begin(cmd, draw, render->layer_count,
                                  kk_layered_bc_bind_layer, render))
      return true;
   vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
   return false;
}

static void
kk_layered_bc_end_draw(struct kk_cmd_buffer *cmd)
{
   if (cmd->state.gfx.render.layered_bc && cmd->state.gfx.layer_replay.bind)
      kk_poly_layer_replay_end(cmd);
}
#endif
