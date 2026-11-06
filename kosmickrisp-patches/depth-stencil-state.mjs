// SPDX-License-Identifier: MIT
import fs from 'node:fs';

/** Export the selected command depth/stencil state without another state owner. */
export const id = 'depth-stencil-state';
export const sourceFiles = [
  'kosmickrisp-patches/kk_depth_stencil_state.h',
  'kosmickrisp-patches/kk_depth_stencil_state.c',
];
export function apply({edit, edits}) {
  for (const extension of ['h', 'c'])
    edits.set('src/kosmickrisp/vulkan/kk_depth_stencil_state.' + extension,
      fs.readFileSync(new URL('./kk_depth_stencil_state.' + extension, import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/meson.build', s => s.replace(
    "  'kk_cmd_draw.c',", "  'kk_cmd_draw.c',\n  'kk_depth_stencil_state.c',"));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s
    .replace('struct kk_attachment {', 'struct kk_depth_stencil_record;\n\nstruct kk_attachment {')
    .replace('   VkAttachmentLoadOp load_op;', '   VkImageLayout image_layout;\n   VkAttachmentLoadOp load_op;')
    .replace('   struct vk_command_buffer vk;',
      '   struct vk_command_buffer vk;\n   struct kk_depth_stencil_record *depth_stencil_records;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kk_depth_stencil_state.h"')
    .replace('      .flags = vk_get_rendering_attachment_flags(info),',
      '      .flags = vk_get_rendering_attachment_flags(info),\n      .image_layout = info->imageLayout,')
    .replace('      mtl_depth_stencil_state *ds = gfx->is_depth_stencil_dynamic\n         ? gfx->depth_stencil_state : kk_attachment_depth_stencil(cmd, vs);',
      '      mtl_depth_stencil_state *ds = kk_cmd_selected_depth_stencil(cmd, vs);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s
    .replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kk_depth_stencil_state.h"')
    .replace('   kk_cmd_release_dynamic_ds_state(cmd);',
      '   kk_cmd_depth_stencil_finish(cmd);\n   kk_cmd_release_dynamic_ds_state(cmd);'));
}
