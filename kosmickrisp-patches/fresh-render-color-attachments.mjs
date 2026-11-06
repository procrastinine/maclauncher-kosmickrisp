// SPDX-License-Identifier: MIT
// New pass descriptors have no previous color attachments to clear.
export const id = 'fresh-render-color-attachments';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', source => source.replace(
    '                         bool rendering_to_whole_framebuffer)\n{\n' +
    '   for (uint32_t i = 0; i < ARRAY_SIZE(render->color_att); i++) {\n' +
    '      mtl_render_pass_attachment_descriptor *attachment_descriptor =\n' +
    '         mtl_render_pass_descriptor_get_color_attachment(pass_descriptor, i);\n' +
    '      kk_clear_common_attachment_description(attachment_descriptor);\n' +
    '   }',
    '                         bool rendering_to_whole_framebuffer,\n' +
    '                         bool clear_previous)\n{\n' +
    '   /* Avoid materializing unused color slots on a new native descriptor.\n' +
    '    * A changed mapping reuses its descriptor and must clear old slots. */\n' +
    '   if (clear_previous) {\n' +
    '      for (uint32_t i = 0; i < ARRAY_SIZE(render->color_att); i++) {\n' +
    '         mtl_render_pass_attachment_descriptor *attachment_descriptor =\n' +
    '            mtl_render_pass_descriptor_get_color_attachment(pass_descriptor, i);\n' +
    '         kk_clear_common_attachment_description(attachment_descriptor);\n' +
    '      }\n' +
    '   }').replace(
    '   kk_set_color_attachments(pass_descriptor, render, dyn, force_attachment_load,\n' +
    '                            is_whole_framebuffer);',
    '   kk_set_color_attachments(pass_descriptor, render, dyn, force_attachment_load,\n' +
    '                            is_whole_framebuffer, false);').replace(
    '            cmd->state.gfx.render.rendering_to_whole_framebuffer);\n' +
    '         needs_restart = true;',
    '            cmd->state.gfx.render.rendering_to_whole_framebuffer, true);\n' +
    '         needs_restart = true;'));
  edit('src/kosmickrisp/vulkan/kk_layered_bc_render.h', source => source.replace(
    '                            force_load, render->rendering_to_whole_framebuffer);',
    '                            force_load, render->rendering_to_whole_framebuffer, true);'));
}
