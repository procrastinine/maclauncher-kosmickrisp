// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', (s, replace) => replace(
 'struct kk_rendering_state {',
 'struct kk_rendering_state {\n   bool attachmentless;'));
edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
 .replace('   if (no_framebuffer) {',
          '   render->attachmentless = no_framebuffer;\n   if (no_framebuffer) {')
 .replace('   bool needs_restart = kk_flush_sample_locations(cmd);', `   bool needs_restart = false;
   /* Metal fixes the default sample count when an encoder starts. Vulkan may
    * change it between attachmentless draws in the same rendering scope. */
   if (render->attachmentless &&
       render->samples != cmd->state.gfx.pipeline_sample_count) {
      render->samples = cmd->state.gfx.pipeline_sample_count;
      mtl_render_pass_descriptor_set_default_raster_sample_count(
         cmd->state.gfx.render_pass_descriptor, render->samples);
      needs_restart = true;
   }
   needs_restart |= kk_flush_sample_locations(cmd);`)
);
edit('src/kosmickrisp/vulkan/kk_physical_device.c', (s, replace) => replace(
 '      .vertexPipelineStoresAndAtomics = true,',
 '      .vertexPipelineStoresAndAtomics = true,\n      .variableMultisampleRate = true,'));
}
