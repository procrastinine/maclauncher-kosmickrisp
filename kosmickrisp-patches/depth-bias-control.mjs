import fs from 'node:fs';
import path from 'node:path';

/** Depth-bias representations use Poly's post-clip framebuffer-depth path. */
export const id = 'depth-bias-control';
export function apply({base: sourceRoot, edits}) {
  const edit = (file, apply) => {
    const before = edits.get(file) ?? fs.readFileSync(path.join(sourceRoot, file), 'utf8');
    const replace = (from, to) => {
      if (!before.includes(from)) throw new Error(`${file}: missing depth-bias anchor`);
      return before.replace(from, to);
    };
    edits.set(file, apply(before, replace));
  };
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .EXT_depth_clip_enable = true,',
      '      .EXT_depth_clip_enable = true,\n      .EXT_depth_bias_control = true,')
    .replace('      .depthBiasClamp = true,', `      .depthBiasClamp = true,
      .depthBiasControl = true,
      .leastRepresentableValueForceUnormRepresentation = true,
      .floatRepresentation = true,
      .depthBiasExact = true,`));
  edit('src/poly/geometry_capture.h', (s, replace) => replace(
    '   uint32_t depth_bias_enable;', `   uint32_t depth_bias_enable;
   uint32_t depth_bias_representation;
   uint32_t polygon_mode;`));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('kk_polygon_possible(const struct vk_graphics_pipeline_state *state)',
      'kk_polygon_possible(const struct kk_device *dev, const struct vk_graphics_pipeline_state *state)')
    .replace('       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE));',
      `       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE) ||
       (dev->vk.enabled_features.depthBiasControl &&
        (state->rs->depth_bias.enable ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) &&
        (state->rs->depth_bias.exact || state->rs->depth_bias.representation !=
            VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS))));`)
    .replace('   nir_def *point = nir_udiv_imm(&b, id, 6);', `   nir_def *mode = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, polygon_mode), 4);
   nir_def *is_point = nir_ieq_imm(&b, mode, VK_POLYGON_MODE_POINT);
   nir_def *point = nir_bcsel(&b, is_point, nir_udiv_imm(&b, id, 6), id);`)
    .replace('         screen = nir_fadd(&b, screen, nir_fadd_imm(&b, coord, -0.5f));',
      `         screen = nir_fadd(&b, screen, nir_bcsel(&b, is_point,
            nir_fadd_imm(&b, coord, -0.5f), nir_imm_vec2(&b, 0, 0)));`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', (s, replace) => replace(
    'pipeline_descriptor, polygon ? MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE : vs->info.vs.topology);',
    'pipeline_descriptor, polygon ? MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED : vs->info.vs.topology);')
    .replaceAll('kk_polygon_possible(state)', 'kk_polygon_possible(dev, state)')
    .replace('   _mesa_blake3_final(&blake3_ctx, blake3_out);', `   /* Representation changes whether a Poly raster pipeline is required.
    * Keep ordinary pipeline cache keys while separating controlled pipelines. */
   if (enabled_features->depthBiasControl && state && state->rs &&
       (state->rs->depth_bias.enable ||
        BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE)) &&
       (state->rs->depth_bias.exact || state->rs->depth_bias.representation !=
           VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT ||
        BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS))) {
      const uint32_t representation_key = 0x44424331;
      _mesa_blake3_update(&blake3_ctx, &representation_key, sizeof(representation_key));
   }
   _mesa_blake3_final(&blake3_ctx, blake3_out);`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('            .znear = 0, .zfar = 1,', `            .znear = dyn->rs.depth_clamp_enable &&
               dyn->rs.polygon_mode != VK_POLYGON_MODE_POINT ? dyn->vp.viewports[i].minDepth : 0,
            .zfar = dyn->rs.depth_clamp_enable &&
               dyn->rs.polygon_mode != VK_POLYGON_MODE_POINT ? dyn->vp.viewports[i].maxDepth : 1,`)
    .replace('   if (point != gfx->polygon_points) {', `   if (point && BITSET_TEST(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE))
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
   if (point != gfx->polygon_points) {`)
    .replace('   bool point = vs->info.polygon_point && dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT;',
      `   /* Poly clips original triangles before applying bias. Native bias is
    * disabled while this path is active, including FILL/LINE representations. */
   bool controlled_bias = dyn->rs.depth_bias.enable &&
      (dyn->rs.depth_bias.exact || dyn->rs.depth_bias.representation !=
         VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT);
   bool point = vs->info.polygon_point &&
      (dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias);`)
    .replace('         .depth_bias_enable = dyn->rs.depth_bias.enable,',
      `         .depth_bias_enable = dyn->rs.depth_bias.enable,
         .depth_bias_representation = dyn->rs.depth_bias.representation,
         .polygon_mode = dyn->rs.polygon_mode,`));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', (s, replace) => replace(
    '      .primitive_type = MTL_PRIMITIVE_TYPE_TRIANGLE,',
    `      .primitive_type = dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE ?
         MTL_PRIMITIVE_TYPE_LINE : MTL_PRIMITIVE_TYPE_TRIANGLE,`));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('   counts[id] = count;', `   counts[id] = p->polygon_mode == 2 ? count :
      p->polygon_mode == 1 ? count * 2 : (count - 2) * 3;`)
    .replace('   p->draw[0] = count * 6;', '   p->draw[0] = count * (p->polygon_mode == 2 ? 6 : 1);')
    .replace('   int power = exponent - (int)p->depth_mantissa_bits;',
      `   int power = exponent - (int)p->depth_mantissa_bits;
   /* FORCE_UNORM uses mantissa bits plus one on floating-point attachments.
    * FLOAT uses one unit of framebuffer depth. Vulkan enum values are 1/2. */
   if (p->depth_bias_representation == 1)
      power = -(int)(p->depth_mantissa_bits + p->depth_is_float);
   else if (p->depth_bias_representation == 2)
      power = 0;`)
    .replace('   uint count = counts[id] - first;', `   uint output_count = counts[id] - first;
   uint count = p->polygon_mode == 2 ? output_count :
      p->polygon_mode == 1 ? output_count / 2 : output_count / 3 + 2;`)
    .replace('   if (!count)\n      return;', '   if (!output_count)\n      return;')
    .replace(`            if (p->depth_clamp)
               z = clamp(z, fmin(p->viewport[viewport][4], p->viewport[viewport][5]),
                            fmax(p->viewport[viewport][4], p->viewport[viewport][5]));`,
      `            if (p->depth_clamp) {
               float minimum = p->viewport[viewport][4], maximum = p->viewport[viewport][5];
               if (p->polygon_mode == 2)
                  z = clamp(z, fmin(minimum, maximum), fmax(minimum, maximum));
               else
                  /* Native clamp follows interpolation, including a polygon
                   * crossing the viewport depth bound after applying bias. */
                  z = minimum == maximum ? 0 : (z - minimum) / (maximum - minimum);
            }`)
    .replace('   for (uint i = 0; i < count; ++i) {\n      struct kk_polygon_vertex vertex = polygon[i];',
      `   for (uint i = 0; i < output_count; ++i) {
      /* Triangle fan or boundary line list. No internal fan edges are drawn
       * in LINE mode, and original flat/provoking values remain unchanged. */
      uint vertex_index = p->polygon_mode == 2 ? i :
         p->polygon_mode == 1 ? (i / 2 + i % 2) % count :
         i % 3 == 0 ? 0 : i / 3 + i % 3;
      struct kk_polygon_vertex vertex = polygon[vertex_index];`));
}
