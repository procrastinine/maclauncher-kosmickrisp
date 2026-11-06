// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'polygon-clip-varyings';
export const sourceFiles = ['kosmickrisp-patches/polygon-scalar-io.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_polygon_scalar_io.h',
    fs.readFileSync(new URL('./polygon-scalar-io.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '#include "kk_polygon_nir.h"',
    '#include "kk_polygon_nir.h"\n#include "kk_polygon_scalar_io.h"'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '         nir_shader *point_vs = kk_polygon_vertex_shader(&shader->info.polygon, nir->options);\n         NIR_PASS(_, point_vs, msl_nir_vs_io_types);',
    '         nir_shader *point_vs = kk_polygon_vertex_shader(&shader->info.polygon, nir->options);\n         NIR_PASS(_, point_vs, kk_nir_lower_polygon_scalar_outputs);\n         NIR_PASS(_, point_vs, msl_nir_vs_io_types);'));
}
