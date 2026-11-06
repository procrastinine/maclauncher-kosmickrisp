// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'layered-block-views';
export const sourceFiles = ['kosmickrisp-layered-bc.h', 'kosmickrisp-layered-bc.c', 'kosmickrisp-layered-bc-nir.h', 'kosmickrisp-layered-bc-render.h', 'kosmickrisp-layered-bc-cube-nir.h'].map(n => 'kosmickrisp-patches/' + n);
export function apply({edit, edits}) {
  for (const [source, target] of [
    ['kosmickrisp-layered-bc.h', 'kk_layered_bc.h'],
    ['kosmickrisp-layered-bc.c', 'kk_layered_bc.c'],
    ['kosmickrisp-layered-bc-nir.h', 'kk_layered_bc_nir.h'],
    ['kosmickrisp-layered-bc-render.h', 'kk_layered_bc_render.h'],
    ['kosmickrisp-layered-bc-cube-nir.h', 'kk_layered_bc_cube_nir.h'],
  ]) edits.set('src/kosmickrisp/vulkan/' + target, fs.readFileSync(new URL(source, import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/meson.build', s => s.replace("  'kk_image_view.c',", "  'kk_image_view.c',\n  'kk_layered_bc.c',"));
  edit('src/kosmickrisp/vulkan/kk_image_view.h', s => s.replace(
    '   struct vk_image_view vk;',
    '   struct vk_image_view vk;\n   struct kk_layered_bc_view *layered_bc;'));
  edit('src/kosmickrisp/vulkan/kk_image_view.c', s => s
    .replace('#include "kk_image_view.h"', '#include "kk_image_view.h"\n#include "kk_layered_bc.h"')
    .replace('         subres_layout.layers = view->vk.layer_count;',
      '         subres_layout.layers = kk_layered_bc_requested(&view->vk) ? 1 : view->vk.layer_count;')
    .replace('         view_layout.base_level = 0u;',
      '         view_layout.base_level = 0u;\n         if (kk_layered_bc_requested(&view->vk))\n            {\n            view_layout.min_lod_clamp = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0f);\n            view_layout.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;\n         }')
    .replace('   VkResult result = kk_writable_msaa_view_init(dev, view);',
      '   VkResult result = kk_writable_msaa_view_init(dev, view);\n   if (result == VK_SUCCESS)\n      result = kk_layered_bc_view_init(dev, view);')
    .replace('   kk_writable_msaa_view_finish(dev, view);',
      '   kk_layered_bc_view_finish(dev, view);\n   kk_writable_msaa_view_finish(dev, view);'));
  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', s => s
    .replace('#include "kk_image_view.h"', '#include "kk_image_view.h"\n#include "kk_layered_bc.h"')
    .replace('         desc[plane].image_min_lod_uint16 = min_lod;',
      '         desc[plane].image_min_lod_uint16 = min_lod;\n         kk_layered_bc_sampled_descriptor(&desc[plane], view);')
    .replace('      kk_sparse_storage_descriptor(&desc, view);',
      '      kk_sparse_storage_descriptor(&desc, view);\n      kk_layered_bc_storage_descriptor(&desc, view);'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => {
    const start = s.indexOf('   struct lower_descriptors_ctx ctx = {', s.indexOf('\nkk_nir_lower_descriptors('));
    const end = s.indexOf('   /* First lower everything', start);
    if (start < 0 || end < 0) throw Error('layered block views: shared descriptor context anchor');
    const setup = s.slice(start, end);
    s = s.slice(0, start) + `   struct lower_descriptors_ctx ctx = kk_descriptor_context(rs, set_layout_count, set_layouts);

` + s.slice(end);
    const common = `static struct lower_descriptors_ctx
kk_descriptor_context(const struct vk_pipeline_robustness_state *rs,
                      uint32_t set_layout_count,
                      struct vk_descriptor_set_layout *const *set_layouts)
{
${setup}   return ctx;
}

#include "kk_layered_bc_nir.h"

bool
kk_nir_prepare_layered_bc_cubes(nir_shader *nir,
                         const struct vk_pipeline_robustness_state *rs,
                         uint32_t set_layout_count,
                         struct vk_descriptor_set_layout *const *set_layouts)
{
   struct lower_descriptors_ctx ctx = kk_descriptor_context(rs, set_layout_count, set_layouts);
   return nir_shader_tex_pass(nir, kk_layered_bc_cube, nir_metadata_none, &ctx);
}

bool
kk_nir_prepare_layered_bc(nir_shader *nir,
                         const struct vk_pipeline_robustness_state *rs,
                         uint32_t set_layout_count,
                         struct vk_descriptor_set_layout *const *set_layouts)
{
   struct lower_descriptors_ctx ctx = kk_descriptor_context(rs, set_layout_count, set_layouts);
   return nir_shader_instructions_pass(nir, kk_layered_bc_instruction,
                                       nir_metadata_none, &ctx);
}

`;
    s = s.replace('bool\nkk_nir_lower_descriptors(', common + 'bool\nkk_nir_lower_descriptors(');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s.replace(
    'bool\nkk_nir_lower_descriptors(',
    `bool kk_nir_prepare_layered_bc_cubes(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts);
bool kk_nir_prepare_layered_bc(nir_shader *nir,
   const struct vk_pipeline_robustness_state *rs, uint32_t set_layout_count,
   struct vk_descriptor_set_layout *const *set_layouts);

bool
kk_nir_lower_descriptors(`));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    'struct kk_rendering_state {',
    'struct kk_rendering_state {\n   bool layered_bc;\n   uint32_t layered_bc_layer;\n   uint32_t layered_bc_view_mask;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
    s = s.replace('#include "kk_image_view.h"',
      '#include "kk_image_view.h"\n#include "kk_layered_bc.h"');
    s = s.replace('      extent->width = MAX2(extent->width, width);',
      '      if (iview->layered_bc) {\n         width = iview->vk.extent.width;\n         height = iview->vk.extent.height;\n      }\n\n      extent->width = MAX2(extent->width, width);');
    const first = s.indexOf('static void\nkk_fill_common_attachment_description(');
    const last = s.indexOf('static struct mtl_clear_color\n', first);
    if (first < 0 || last < 0) throw Error('layered block views: attachment helper');
    let fill = s.slice(first, last);
    fill = fill.replace('   const struct kk_attachment *info, bool force_attachment_load,',
      '   const struct kk_attachment *info, const struct kk_rendering_state *render,\n   bool force_attachment_load,');
    fill = fill.replace('   /* STORE_OP_NONE behaves',
      '   kk_layered_bc_attachment_description(descriptor, info, render, false);\n\n   /* STORE_OP_NONE behaves');
    const tail = fill.lastIndexOf('   }\n}');
    if (tail < 0) throw Error('layered block views: resolve helper');
    fill = fill.slice(0, tail) +
      '      kk_layered_bc_attachment_description(descriptor, info, render, true);\n' +
      fill.slice(tail);
    s = s.slice(0, first) + fill + s.slice(last);
    s = s.replace('kk_fill_common_attachment_description(attachment_descriptor, color_att,',
      'kk_fill_common_attachment_description(attachment_descriptor, color_att, render,');
    for (const aspect of ['depth', 'stencil'])
      s = s.replace(`attachment_descriptor, &render->${aspect}_att, force_attachment_load,`,
        `attachment_descriptor, &render->${aspect}_att, render, force_attachment_load,`);
    s = s.replace('static void\nkk_begin_rendering(',
      '#include "kk_layered_bc_render.h"\n\nstatic void\nkk_begin_rendering(');
    s = s.replace('   VkResult result = kk_transient_prepare_rendering(cmd, pRenderingInfo);',
      `   const bool layered_bc = kk_layered_bc_rendering_info_requested(pRenderingInfo);
   /* Per-layer physical passes cannot use whole-pass memoryless retention.
    * Reuse the existing private commitment path for paired transient views. */
   const bool transient_safe = cmd->transient_safe;
   cmd->transient_safe &= !layered_bc;
   VkResult result = kk_transient_prepare_rendering(cmd, pRenderingInfo);
   cmd->transient_safe = transient_safe;`);
    s = s.replace('   render->flags = pRenderingInfo->flags;',
      '   render->layered_bc = layered_bc;\n   render->layered_bc_view_mask = pRenderingInfo->viewMask;\n   render->flags = pRenderingInfo->flags;');
    s = s.replace('         pass_descriptor, layer_count ? layer_count : 1u);',
      '         pass_descriptor, render->layered_bc ? 1u : (layer_count ? layer_count : 1u));');
    s = s.replace('   if (!no_framebuffer)\n      cs_start_render(cmd);',
      '   if (!no_framebuffer && !kk_layered_bc_start_rendering(cmd, force_attachment_load))\n      return;');
    s = s.replace('   cmd->state.gfx.need_to_start_render_pass = no_framebuffer;',
      '   cmd->state.gfx.need_to_start_render_pass = no_framebuffer || render->layered_bc;');
    const drawStart = s.indexOf('static void\nkk_draw_one_view(');
    const drawEnd = s.indexOf('\nstatic void\nkk_draw(', drawStart);
    if (drawStart < 0 || drawEnd < 0) throw Error('layered block views: draw cleanup boundary');
    let draw = s.slice(drawStart, drawEnd).replaceAll('return;', 'goto layered_bc_done;');
    draw = draw.replace('   kk_sparse_refresh_shader(cmd, MESA_SHADER_VERTEX);',
      '   if (!kk_layered_bc_prepare_draw(cmd)) return;\n   kk_sparse_refresh_shader(cmd, MESA_SHADER_VERTEX);');
    draw = draw.replace('   kk_flush_gfx_state(cmd);\n\n   data->restart',
      '   kk_flush_gfx_state(cmd);\n   if (!kk_layered_bc_begin_draw(cmd, data)) goto layered_bc_done;\n\n   data->restart');
    const closing = draw.lastIndexOf('}');
    draw = draw.slice(0, closing) +
      '\nlayered_bc_done:\n   kk_layered_bc_end_draw(cmd);\n' + draw.slice(closing);
    s = s.slice(0, drawStart) + draw + s.slice(drawEnd);
    /* Indirect draws with alias attachments use the prepared raster path. */
    s = s.replaceAll('   if (pInfo->maxDrawCount >= 1024 &&',
      '   if (!cmd->state.gfx.render.layered_bc && pInfo->maxDrawCount >= 1024 &&');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_mesh_command.h', s => {
    const start = s.indexOf('static void\nkk_draw_mesh(');
    const end = s.indexOf('VKAPI_ATTR void VKAPI_CALL\nkk_CmdDrawMeshTasksEXT(', start);
    if (start < 0 || end < 0) throw Error('layered block views: Mesh original draw boundary');
    let draw = s.slice(start, end).replaceAll('return;', 'goto layered_bc_mesh_done;');
    const body = draw.indexOf('{');
    draw = draw.slice(0, body + 1) + `
   if (cmd->state.gfx.render.layered_bc) {
      if (!kk_layered_bc_prepare_draw(cmd)) return;
      kk_flush_gfx_state(cmd);
      if (!kk_mesh_layer_replay_begin(cmd, cmd->state.gfx.render.layer_count,
            kk_layered_bc_bind_layer, &cmd->state.gfx.render))
         goto layered_bc_mesh_error;
   }
` + draw.slice(body + 1);
    const close = draw.lastIndexOf('}');
    draw = draw.slice(0, close) + `
   goto layered_bc_mesh_done;
layered_bc_mesh_error:
   vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
layered_bc_mesh_done:
   if (cmd->state.gfx.render.layered_bc && cmd->state.gfx.layer_replay.bind)
      kk_mesh_layer_replay_end(cmd);
` + draw.slice(close);
    return s.slice(0, start) + draw + s.slice(end);
  });
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '   if (features & BITFIELD_BIT(KK_FEAT_NON_SEAMLESS_CUBE))',
    '   NIR_PASS(_, nir, kk_nir_prepare_layered_bc_cubes, rs, set_layout_count, set_layouts);\n   if (features & BITFIELD_BIT(KK_FEAT_NON_SEAMLESS_CUBE))').replace(
    '   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers,',
    '   NIR_PASS(_, nir, kk_nir_prepare_layered_bc, rs, set_layout_count, set_layouts);\n   NIR_PASS(_, nir, kk_nir_lower_sampler_helpers,'));
}
