// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'fragment-offset-planes';
export const sourceFiles = ['kosmickrisp-patches/fragment-offset-planes.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_fragment_offset_planes.h',
    fs.readFileSync(new URL('./fragment-offset-planes.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c', s => s
    .replace('#include "nir_builder.h"',
      '#include "nir_builder.h"\n#include "kk_fragment_offset_planes.h"')
    .replace('   map->active = true;\n   gather_flat_locations(impl, map);',
      '   /* Run after per-vertex lowering has introduced its basis inputs. */\n   kk_nir_lower_fs_offset_planes(fs);\n   map->active = true;\n   gather_flat_locations(impl, map);'));
}
