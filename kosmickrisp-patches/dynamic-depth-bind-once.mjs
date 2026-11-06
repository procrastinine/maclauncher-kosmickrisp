// SPDX-License-Identifier: MIT
// Coalesce the two depth/stencil bindings in a combined pipeline flush.
export const id = 'dynamic-depth-bind-once';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', source => source.replace(
    '   /* Depth/stencil state may be dynamic, handle it as part of the pipeline. */',
    '   bool bind_dynamic_ds = false;\n\n' +
    '   /* Depth/stencil state may be dynamic, handle it as part of the pipeline. */').replace(
    '         device, &dyn->ds, has_depth, has_stencil);\n' +
    '      mtl_set_depth_stencil_state(enc, gfx->depth_stencil_state);',
    '         device, &dyn->ds, has_depth, has_stencil);\n' +
    '      bind_dynamic_ds = true;').replace(
    '      if (!pipeline)\n         return;\n      mtl_render_set_pipeline_state(enc, pipeline);',
    '      if (!pipeline) {\n' +
    '         if (bind_dynamic_ds)\n' +
    '            mtl_set_depth_stencil_state(enc, gfx->depth_stencil_state);\n' +
    '         return;\n      }\n      mtl_render_set_pipeline_state(enc, pipeline);').replace(
    '      if (ds)\n         mtl_set_depth_stencil_state(enc, ds);\n' +
    '   }\n\n   /* Merge tess info',
    '      if (ds || bind_dynamic_ds)\n         mtl_set_depth_stencil_state(enc, ds);\n' +
    '   } else if (bind_dynamic_ds) {\n' +
    '      mtl_set_depth_stencil_state(enc, gfx->depth_stencil_state);\n' +
    '   }\n\n   /* Merge tess info'));
}
