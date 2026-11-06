// SPDX-License-Identifier: MIT

import fs from 'node:fs';

export const id = 'shader-quad-control';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-quad-control.h'];

// NIR retains RequireFullQuads/QuadDerivatives execution modes. Quad votes
// use Kosmic's existing active-lane ballot reduction: terminated lanes leave
// the ballot and divergent votes include only participating invocations.
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/compiler/kk_quad_control.h', fs.readFileSync(new URL('./kosmickrisp-quad-control.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/compiler/msl_nir_lower_subgroups.c', s => s
    .replace('#include "nir_builder.h"', '#include "nir_builder.h"\n#include "kk_quad_control.h"')
    .replace('      .lower_subgroup_masks = true,', `      .lower_quad_vote = nir->info.stage == MESA_SHADER_FRAGMENT,
      .filter = nir->info.stage == MESA_SHADER_FRAGMENT ? kk_quad_control_filter : NULL,
      .filter_data = nir,
      .lower_subgroup_masks = true,`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .KHR_shader_maximal_reconvergence = true,',
      '      .KHR_shader_maximal_reconvergence = true,\n' +
      '      .KHR_shader_quad_control = true,')
    .replace('      .shaderMaximalReconvergence = true,',
      '      .shaderMaximalReconvergence = true,\n' +
      '      .shaderQuadControl = true,'));
}
