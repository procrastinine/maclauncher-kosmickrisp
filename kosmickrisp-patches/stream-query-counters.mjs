// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('         params.prims_generated_counter[i] = gfx->xfb_query[i] + sizeof(uint64_t);',
      '         params.prims_generated_counter_secondary[i] = gfx->xfb_query[i] + sizeof(uint64_t);'));
  // Pre-GS is a single invocation ordered between draws. Accumulate directly
  // into complete Vulkan reports, including when both query types are active.
  edit('src/poly/geometry.h', s => s
    .replace('   DEVICE(uint) prims_generated_counter[POLY_MAX_VERTEX_STREAMS];',
      '   DEVICE(uint64_t) prims_generated_counter[POLY_MAX_VERTEX_STREAMS];\n   DEVICE(uint64_t) prims_generated_counter_secondary[POLY_MAX_VERTEX_STREAMS];')
    .replace('   DEVICE(uint) xfb_prims_generated_counter[POLY_MAX_VERTEX_STREAMS];',
      '   DEVICE(uint64_t) xfb_prims_generated_counter[POLY_MAX_VERTEX_STREAMS];')
    .replace('sizeof(struct poly_geometry_params) == 82 * 4',
      'sizeof(struct poly_geometry_params) == 90 * 4')
    .replace('struct poly_geometry_params must be 82 words',
      'struct poly_geometry_params must be 90 words'));
  edit('src/poly/cl/geometry.cl', s => s.replace(
    '      *(p->prims_generated_counter[i]) += in_prims[i];',
    `      *(p->prims_generated_counter[i]) += in_prims[i];
      if (p->prims_generated_counter_secondary[i])
         *(p->prims_generated_counter_secondary[i]) += in_prims[i];`)
);
}
