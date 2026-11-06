// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
 s=s.replace('framebuffer_extent.width = render->area.extent.width;',
   'framebuffer_extent.width = MAX2(render->area.extent.width, 1u);');
 s=s.replace('framebuffer_extent.height = render->area.extent.height;',
   'framebuffer_extent.height = MAX2(render->area.extent.height, 1u);');
 return s;
});
}
