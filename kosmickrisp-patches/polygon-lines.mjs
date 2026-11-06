// SPDX-License-Identifier: MIT
export function apply({edit}) {
// Metal keeps triangle fill mode on the encoder, including after Poly stages.
edit('src/kosmickrisp/bridge/mtl_encoder.h', s => s.replace('#include <stdint.h>', '#include <stdint.h>\n#include <stdbool.h>').replace(
 'void mtl_set_cull_mode(mtl_render_encoder *encoder, enum mtl_cull_mode mode);',
 `void mtl_set_cull_mode(mtl_render_encoder *encoder, enum mtl_cull_mode mode);
void mtl_set_triangle_fill_lines(mtl_render_encoder *encoder, bool lines);`));
edit('src/kosmickrisp/bridge/mtl_encoder.m', (s, replace) => replace(
 'void\nmtl_set_visibility_result_mode(',
 `void
mtl_set_triangle_fill_lines(mtl_render_encoder *encoder, bool lines)
{
   @autoreleasepool {
      id<MTL4RenderCommandEncoder> enc = (id<MTL4RenderCommandEncoder>)encoder;
      [enc setTriangleFillMode:lines ? MTLTriangleFillModeLines
                                     : MTLTriangleFillModeFill];
   }
}

void
mtl_set_visibility_result_mode(`));
edit('src/kosmickrisp/bridge/stubs/mtl_encoder.c', (s, replace) => replace(
 'void\nmtl_set_visibility_result_mode(',
 `void
mtl_set_triangle_fill_lines(mtl_render_encoder *encoder, bool lines)
{
}

void
mtl_set_visibility_result_mode(`));
edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
 .replace('   if (IS_DIRTY(RS_DEPTH_BIAS_FACTORS) || IS_DIRTY(RS_DEPTH_BIAS_ENABLE)) {',
 `   if (IS_DIRTY(RS_POLYGON_MODE))
      mtl_set_triangle_fill_lines(enc, dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE);

   if (IS_DIRTY(RS_DEPTH_BIAS_FACTORS) || IS_DIRTY(RS_DEPTH_BIAS_ENABLE)) {`));
}
