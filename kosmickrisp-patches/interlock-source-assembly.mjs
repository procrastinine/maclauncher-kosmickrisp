// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const sourceFiles = [
  'kosmickrisp-patches/interlock-source-assembly.mjs',
  'kosmickrisp-patches/kosmickrisp-interlock-source.cl',
  'kosmickrisp-patches/kosmickrisp-interlock-source-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-source-command.h',
  'kosmickrisp-patches/kosmickrisp-interlock-source-distance.h',
];

/* Apply after fragment-interlock-envelope. Shared capture stores the same
 * once-only GS outputs for every primitive topology. Selected clipping/output
 * uses bounded full threadgroups; triangle assembly and prefix remain shared. */
export function apply({edit, edits}) {
  const owned = name => fs.readFileSync(new URL('./' + name, import.meta.url), 'utf8');
  for (const [target, name] of [
    ['libkk/kk_interlock_source.cl', 'kosmickrisp-interlock-source.cl'],
    ['vulkan/kk_interlock_source_nir.h', 'kosmickrisp-interlock-source-nir.h'],
    ['vulkan/kk_interlock_source_command.h', 'kosmickrisp-interlock-source-command.h'],
    ['libkk/kk_interlock_source_distance.h', 'kosmickrisp-interlock-source-distance.h'],
  ]) edits.set('src/kosmickrisp/' + target, owned(name));

  // The accepted shared capture already handles point/line arity, separate
  // cull IO and bounded full threadgroups. Preserve those common semantics.
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => {
    s = s.replace(`KERNEL(1)
libkk_polygon_capture_setup(global struct poly_geometry_params *geometry,
                            global struct poly_heap *heap)`,
      `static void
polygon_capture_setup(global struct poly_geometry_params *geometry,
                       global struct poly_heap *heap, uint arity, bool interlock)`);
    s = s.replace('uint triangles = streams * sub_sat(p->vertices_out, p->primitive_vertices - 1u);',
      'uint triangles = streams * sub_sat(p->vertices_out, arity - 1u);');
    s = s.replace('   p->clipped = (uintptr_t)poly_heap_alloc(heap, triangles * 8, 8);',
      `   uint clipped_size = triangles * 8;
   if (interlock && arity < 3)
      clipped_size = align(clipped_size, 16u) +
         triangles * arity * sizeof(struct kk_polygon_vertex);
   p->clipped = (uintptr_t)poly_heap_alloc(heap, clipped_size, interlock && arity < 3 ? 16 : 8);`);
    const original = `KERNEL(1)
libkk_polygon_capture_setup(global struct poly_geometry_params *geometry,
                            global struct poly_heap *heap)
{
   polygon_capture_setup(geometry, heap,
      polygon_capture(geometry)->primitive_vertices, false);
}

`;
    s = s.replace('static bool\npolygon_triangle(', original + 'static bool\npolygon_triangle(');
    return s + '\n#include "kk_interlock_source.cl"\n';
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    '#include "kk_polygon_command.h"',
    '#include "kk_interlock_source_command.h"\n#include "kk_polygon_command.h"'));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s
    .replace('   libkk_polygon_capture_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));',
      `   if (cmd->interlock_heap_active)
      kk_interlock_source_setup(cmd, geometry_params);
   else
      libkk_polygon_capture_setup(cmd, kk_grid_1d(1), true, geometry_params, kk_heap(cmd));`)
    .replace('   struct kk_grid triangles = kk_grid_indirect(capture +',
      `   if (cmd->interlock_heap_active && kk_interlock_source_arity(cmd) < 3)
      return kk_interlock_source_assemble(cmd, geometry_params);

   struct kk_grid triangles = kk_grid_indirect(capture +`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('#include "kk_polygon_nir.h"',
      '#include "kk_polygon_nir.h"\n#include "kk_interlock_source_nir.h"')
    .replace('nir_shader *point_vs = kk_polygon_vertex_shader(&shader->info.polygon, nir->options,',
      `nir_shader *point_vs = pixel_envelope && mesa_vertices_per_prim(shader->info.gs.mode) < 3 ?
            kk_interlock_source_vertex_shader(&shader->info.polygon, nir->options,
               mesa_vertices_per_prim(shader->info.gs.mode)) :
            kk_polygon_vertex_shader(&shader->info.polygon, nir->options,`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '         NIR_PASS(_, point_vs, msl_nir_vs_io_types);',
    `         if (pixel_envelope)
            kk_interlock_source_resolved_cull(point_vs, &shader->info.polygon);
         NIR_PASS(_, point_vs, msl_nir_vs_io_types);`));
}
