// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'poly-retained-points';
export const sourceFiles = ['kosmickrisp-patches/poly-retained-points-nir.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_poly_retained_points_nir.h',
    fs.readFileSync(new URL('./poly-retained-points-nir.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('static nir_shader *\nkk_polygon_vertex_shader(',
      '#include "kk_poly_retained_points_nir.h"\n\nstatic nir_shader *\nkk_polygon_vertex_shader(')
    .replace('   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,\n                                                  "Poly polygon points");',
      `   if (info->primitive_vertices == 1)
      return kk_polygon_retained_point_shader(info, options);
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options,
                                                  "Poly polygon points");`)
    .replace(/   if \(info->primitive_vertices == 1\) \{\n[\s\S]*?      return nir;\n   }\n/, ''));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    '         .line_expand = capture->primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE,',
    '         .line_expand = capture->primitive_vertices == 2 ||\n            (capture->primitive_vertices == 3 && dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE),'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('      bool polygon = (kk_polygon_possible(dev, state) || opts->poly_layer_replay) &&',
      `      bool retained_points = opts->poly_layer_replay &&
         u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_POINTS;
      bool polygon = retained_points || ((kk_polygon_possible(dev, state) || opts->poly_layer_replay) &&`)
    .replace('          u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_POINTS);',
      '          u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_POINTS));')
    .replace('         nir_shader *point_vs = pixel_envelope && mesa_vertices_per_prim(shader->info.gs.mode) < 3 ?',
      `         nir_shader *point_vs = retained_points ?
            kk_polygon_retained_point_shader(&shader->info.polygon, nir->options) :
            pixel_envelope && mesa_vertices_per_prim(shader->info.gs.mode) < 3 ?`)
    .replace(/      \/\* Point output keeps Poly's ordinary cached GS reader and point size\. \*\/\n      if \(opts->poly_layer_replay && !polygon\) \{[\s\S]*?kk_compile_msl\(pdev, layer_vs, state, &shader->polygon\[2\]\);\n      }/,
      `      /* Layered point output uses the retained reader compiled above.
       * Never clone this ordinary raster body for physical-layer replay. */`));
}
