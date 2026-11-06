// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'geometry-raster-bounds';
export const sourceFiles = ['kosmickrisp-patches/geometry-raster-bounds.h'];
export function apply({edit, edits}) {
  edits.set('src/poly/nir/poly_gs_raster_bounds.h',
    fs.readFileSync(new URL('./geometry-raster-bounds.h', import.meta.url),'utf8'));
  edit('src/poly/nir/poly_nir_lower_gs.c', s => s
    .replace('static nir_shader *\ncreate_gs_rast_shader(',
      '#include "poly_gs_raster_bounds.h"\n\nstatic nir_shader *\ncreate_gs_rast_shader(')
    .replace('   nir_shader_intrinsics_pass(shader, collect_output_types, nir_metadata_all,',
      '   nir_cursor raster_body_start = b->cursor;\n\n   nir_shader_intrinsics_pass(shader, collect_output_types, nir_metadata_all,')
    .replace('   return shader;\n}\n\nstatic bool\nlower_gs_instr(',
      '   poly_guard_gs_raster(shader, gs, &rs, raster_body_start);\n   return shader;\n}\n\nstatic bool\nlower_gs_instr('));
}
