// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const sourceFiles = [
  'kosmickrisp-patches/interlock-layers.mjs',
  'kosmickrisp-patches/kosmickrisp-interlock-layers-nir.h',
];

export function applyInterlockLayers({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_interlock_layers_nir.h',
    fs.readFileSync(new URL('./kosmickrisp-interlock-layers-nir.h', import.meta.url), 'utf8'));
  const replace = (source, from, to) => {
    if (!source.includes(from)) throw Error('Missing interlock layer anchor: ' + from);
    return source.replace(from, to);
  };
  edit('src/kosmickrisp/vulkan/kk_shader.c', source => {
    source = replace(source, '#include "kk_interlock_select.h"',
      '#include "kk_interlock_select.h"\n#include "kk_interlock_layers_nir.h"');
    return replace(source, '   gather_shader_info(shader, nir, state);',
      `   if (pixel_envelope && stage != MESA_SHADER_FRAGMENT && state->mv && state->mv->view_mask) {
      NIR_PASS(_, nir, kk_interlock_lower_source_views);
      NIR_PASS(_, nir, kk_nir_lower_descriptors, false, robustness, 0, NULL);
   }
   gather_shader_info(shader, nir, state);`);
  });
}
