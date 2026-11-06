// SPDX-License-Identifier: MIT
// Per-view Poly replay follows pinned Honeykrisp (Mesa 4f071f89, MIT).
import fs from 'node:fs';
export const id = 'multiview-poly';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-multiview-nir.h'];

export function apply({edit, edits}) {
  const change = (file, apply) => edit('src/kosmickrisp/vulkan/' + file, before => {
    const replace = (s, from, to) => {
      if (!s.includes(from)) throw new Error(`${file}: missing Poly multiview anchor ${from.slice(0, 70)}`);
      return s.replace(from, to);
    };
    return apply(before, replace);
  });
  edits.set('src/kosmickrisp/vulkan/kk_multiview_nir.h',
    fs.readFileSync(new URL('./kosmickrisp-multiview-nir.h', import.meta.url), 'utf8'));

  change('kk_cmd_buffer.h', (s, r) => r(s,
    '   uint64_t per_vertex_heap;\n};',
    `   uint64_t per_vertex_heap;
   /* Optional Poly multiview tail. Ordinary draws do not upload it. */
   uint64_t multiview_index;
};`));

  change('kk_shader.c', (s, r) => {
    s = r(s, '#include "poly/geometry.h"',
      '#include "poly/geometry.h"\n#include "kk_multiview_nir.h"');
    s = r(s, '   const uint32_t feature_key[] = {',
      `   /* Pre-raster multiview also changes shaders without an FS. Keep
    * ordinary no-multiview keys unchanged. */
   if (!(stages & VK_SHADER_STAGE_FRAGMENT_BIT) && state && state->mv &&
       state->mv->view_mask)
      _mesa_blake3_update(&blake3_ctx, &state->mv->view_mask,
                          sizeof(state->mv->view_mask));

   const uint32_t feature_key[] = {`);
    s = r(s, '   /* Massage IO related variables to please Metal */',
      `   bool poly_preraster = state &&
      (state->shader_stages & (VK_SHADER_STAGE_GEOMETRY_BIT |
                              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
   bool poly_multiview = poly_preraster && state->mv->view_mask;
   bool last_preraster = nir->info.stage == MESA_SHADER_GEOMETRY ||
      (nir->info.stage == MESA_SHADER_TESS_EVAL &&
       !(state->shader_stages & VK_SHADER_STAGE_GEOMETRY_BIT));
   /* Scan actual intrinsics: early system_values_read predates MSL
    * preprocessing and cannot gate ViewIndex lowering for these stages. */
   if (poly_preraster && nir->info.stage != MESA_SHADER_FRAGMENT)
      NIR_PASS(_, nir, kk_nir_lower_poly_multiview, poly_multiview,
               last_preraster);

   /* Massage IO related variables to please Metal */`);
    s = r(s,
      '      NIR_PASS(_, nir, kk_nir_lower_vs_multiview, state->mv->view_mask);',
      `      if (!poly_multiview)
         NIR_PASS(_, nir, kk_nir_lower_vs_multiview, state->mv->view_mask);`);
    return r(s, '      info->vs.view_mask = state->mv->view_mask;',
      `      bool poly_multiview = state->mv->view_mask &&
         (state->shader_stages & (VK_SHADER_STAGE_GEOMETRY_BIT |
                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
      /* Poly already replays the stages for one view at a time. */
      info->vs.view_mask = poly_multiview ? 1u : state->mv->view_mask;`);
  });

  change('kk_cmd_buffer.c', (s, r) => r(s,
    '   uint32_t view_mask = state->render.view_mask;',
    `   uint32_t view_mask = state->render.view_mask;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   if (view_mask && vs && (vs->info.api_geometry ||
       cmd->state.shaders[MESA_SHADER_TESS_EVAL]))
      view_mask = 0; /* A Poly replay writes its explicit layer. */`));

  change('kk_cmd_meta.c', (s, r) => {
    s = r(s,
      '   cmd->state.cond_render = cond_render;\n   vk_meta_clear_attachments',
      `   cmd->state.cond_render = cond_render;
   /* vk_meta expands one explicit-layer rectangle for each original view.
    * Preserve its single-amplification draw while leaving render_info intact. */
   cmd->state.gfx.render.view_mask = 0;
   vk_meta_clear_attachments`);
    return r(s,
      '                             attachmentCount, pAttachments, rectCount, pRects);\n   kk_meta_end',
      `                             attachmentCount, pAttachments, rectCount, pRects);
   cmd->state.gfx.render.view_mask = view_mask;
   kk_meta_end`);
  });

  change('kk_cmd_draw.c', (s, r) => {
    s = r(s,
      '   uint32_t per_draw_size = per_vertex ? sizeof(gfx->per_draw_data) :',
      `   bool multiview = gfx->render.view_mask &&
      (cmd->state.shaders[MESA_SHADER_TESS_EVAL] ||
       cmd->state.shaders[MESA_SHADER_VERTEX]->info.api_geometry);
   uint32_t per_draw_size = multiview ? sizeof(gfx->per_draw_data) :
      per_vertex ? offsetof(struct kk_per_draw_data, multiview_index) :`);
    s = r(s, 'kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)\n{',
      'kk_draw_one_view(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)\n{');
    s = r(s, '      kk_dispatch_draw(cmd, draw_data);',
      `      if (gfx->render.view_mask) {
         uint32_t layers[KK_MAX_MULTIVIEW_VIEW_COUNT] = {0};
         uint32_t count = 0;
         if (tess || vs->info.api_geometry) {
            count = 1;
         } else {
            u_foreach_bit(view, gfx->render.view_mask)
               layers[count++] = view;
         }
         mtl_set_vertex_amplification_count(cs_get_render(cmd), layers, count);
      }
      kk_dispatch_draw(cmd, draw_data);`);
    return r(s, '\nVKAPI_ATTR void VKAPI_CALL\nkk_CmdDraw(',
      `
static void
kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   uint32_t view_mask = gfx->render.view_mask;
   if (!view_mask || !(vs->info.api_geometry ||
                      cmd->state.shaders[MESA_SHADER_TESS_EVAL])) {
      kk_draw_one_view(cmd, data);
      return;
   }

   /* As in Honeykrisp, replay actual work for each view. A one-bit mask keeps
    * every real query producer at one view per replay; EndQuery publishes the
    * summed first query plus the reset zero entries for the other views. */
   u_foreach_bit(view, view_mask) {
      gfx->render.view_mask = BITFIELD_BIT(view);
      gfx->per_draw_data.multiview_index = view;
      struct kk_draw_command per_view = *data;
      kk_draw_one_view(cmd, &per_view);
   }
   gfx->render.view_mask = view_mask;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDraw(`);
  });

  change('kk_physical_device.c', (s, r) => r(s,
    '      .multiview = true,',
    `      .multiview = true,
      .multiviewGeometryShader = true,
      .multiviewTessellationShader = true,`));
}
