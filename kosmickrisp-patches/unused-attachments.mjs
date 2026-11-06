// SPDX-License-Identifier: MIT
import fs from 'node:fs';

// Attachment-keyed Metal pipelines follow the technique inspected in
// metalsharp/VKD3D-Proton-MacOS 4e89065e359f9c6c2a78170b045835d6382113d9,
// patches/moltenvk/metalsharp-vkmt.diff, MVKGraphicsPipeline attachment variants.
// The implementation here uses Kosmic's MTL4 descriptors and shader lifetime.
export const id = 'unused-attachments';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-unused-attachments.h'];

export function apply({edits, edit: editSource}) {
  const edit = (file, transform) => editSource('src/kosmickrisp/' + file, transform);
  const replace = (s, from, to) => {
    if (!s.includes(from)) throw Error('Missing unused-attachment anchor: ' + from.slice(0, 100));
    return s.replace(from, to);
  };
  edits.set('src/kosmickrisp/vulkan/kk_unused_attachments.h',
    fs.readFileSync(new URL('./kosmickrisp-unused-attachments.h', import.meta.url), 'utf8'));

  edit('vulkan/kk_shader.h', s => replace(s, '   struct kk_pipeline_handles pipeline;',
    '   struct kk_attachment_pipelines *attachment_pipelines;\n   struct kk_pipeline_handles pipeline;')
    .replace('void kk_cmd_bind_compute_shader(', `mtl_render_pipeline_state *kk_attachment_pipeline(struct kk_cmd_buffer *cmd,
   struct kk_shader *shader, bool polygon);
mtl_depth_stencil_state *kk_attachment_depth_stencil(struct kk_cmd_buffer *cmd,
   struct kk_shader *shader);

void kk_cmd_bind_compute_shader(`));

  edit('vulkan/kk_shader.c', s => {
    s = replace(s, '#include "vk_pipeline.h"',
      '#include "vk_pipeline.h"\n#include "kk_image_view.h"\n#include "kk_unused_attachments.h"');
    s = replace(s, '   struct kk_pipeline_handles *pipe = &shader->pipeline;',
      '   kk_attachment_pipelines_finish(shader);\n   struct kk_pipeline_handles *pipe = &shader->pipeline;');
    s = replace(s, '   mtl_release(pipeline_descriptor);', `   if (render && device->vk.enabled_features.dynamicRenderingUnusedAttachments) {
      if (!vs->attachment_pipelines) {
         vs->attachment_pipelines = calloc(1, sizeof(*vs->attachment_pipelines));
         if (!vs->attachment_pipelines)
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
         else
            simple_mtx_init(&vs->attachment_pipelines->mutex, mtx_plain);
      }
      if (vs->attachment_pipelines)
         vs->attachment_pipelines->descriptor[polygon] = mtl_retain(pipeline_descriptor);
   }
   mtl_release(pipeline_descriptor);`);
    s = replace(s, '   /* Fold constant offset srcs for IO. */\n   NIR_PASS(_, nir, nir_opt_constant_folding);\n   NIR_PASS(_, nir, nir_lower_blend, &opts);',
      `   /* Undefined pipeline formats discard stores even when a pass binds a
    * real attachment there. Remove them before Metal types the outputs. */
   nir_shader_intrinsics_pass(nir, kk_discard_unused_color,
                              nir_metadata_control_flow, &opts);
   /* Fold constant offset srcs for IO. */
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_lower_blend, &opts);`);
    return s;
  });

  edit('vulkan/kk_cmd_draw.c', s => {
    s = replace(s, 'IS_DIRTY(DS_STENCIL_WRITE_MASK))) {',
      'IS_DIRTY(DS_STENCIL_WRITE_MASK) || IS_SHADER_DIRTY(VERTEX))) {');
    s = replace(s, '      bool has_depth = dyn->rp.attachments & MESA_VK_RP_ATTACHMENT_DEPTH_BIT;',
      `      struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
      bool has_depth = gfx->render.depth_att.iview &&
         vs->info.vs.d_format != MTL_PIXEL_FORMAT_INVALID;`);
    s = replace(s, '      bool has_stencil =\n         dyn->rp.attachments & MESA_VK_RP_ATTACHMENT_STENCIL_BIT;',
      `      bool has_stencil = gfx->render.stencil_att.iview &&
         vs->info.vs.s_format != MTL_PIXEL_FORMAT_INVALID;`);
    s = replace(s, '   if (IS_SHADER_DIRTY(VERTEX) || IS_DIRTY(RS_POLYGON_MODE)) {',
      `   if (IS_SHADER_DIRTY(VERTEX) || IS_DIRTY(RS_POLYGON_MODE) ||
       IS_DIRTY(CB_COLOR_WRITE_ENABLES) || IS_DIRTY(COLOR_ATTACHMENT_MAP)) {`);
    s = replace(s, '      mtl_render_set_pipeline_state(enc, gfx->polygon_points ? vs->pipeline.gfx.polygon_render : vs->pipeline.gfx.render);',
      `      mtl_render_pipeline_state *pipeline =
         kk_attachment_pipeline(cmd, vs, gfx->polygon_points);
      if (!pipeline)
         return;
      mtl_render_set_pipeline_state(enc, pipeline);`);
    s = replace(s, '      if (gfx->depth_stencil_state)\n         mtl_set_depth_stencil_state(enc, gfx->depth_stencil_state);',
      `      mtl_depth_stencil_state *ds = gfx->is_depth_stencil_dynamic
         ? gfx->depth_stencil_state : kk_attachment_depth_stencil(cmd, vs);
      if (ds)
         mtl_set_depth_stencil_state(enc, ds);`);
    return s;
  });
  edit('bridge/mtl_compiler.h', s => replace(s, '#endif /* MTL_COMPILER_H */',
    `mtl_render_pipeline_state *mtl_new_render_pipeline_for_attachments(
   mtl_compiler *compiler, mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled);

#endif /* MTL_COMPILER_H */`));
  edit('bridge/mtl_compiler.m', s => s + `
/* Copy only Metal state; shader functions and pre-raster programs are shared. */
mtl_render_pipeline_state *
mtl_new_render_pipeline_for_attachments(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)descriptor copy];
      for (uint32_t i = 0; i < count; ++i) {
         copy.colorAttachments[i].pixelFormat = (MTLPixelFormat)formats[i];
         if (disabled & (1u << i))
            copy.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
      }
      mtl_render_pipeline_state *pipeline = mtl_new_render_pipeline(compiler, (void *)copy);
      [copy release];
      return pipeline;
   }
}
`);
  edit('vulkan/kk_physical_device.c', s => replace(s,
    '   .EXT_extended_dynamic_state = true,',
    '   .EXT_dynamic_rendering_unused_attachments = true,\n   .EXT_extended_dynamic_state = true,')
    .replace('      .dynamicRendering = true,',
      '      .dynamicRendering = true,\n      .dynamicRenderingUnusedAttachments = true,'));
}
