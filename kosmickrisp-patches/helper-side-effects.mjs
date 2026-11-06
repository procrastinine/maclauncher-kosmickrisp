// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'helper-side-effects';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-helper-side-effects.h'];

export function apply({edit, edits}) {
  // Preserve older-OS write guards, but resolve their queries only after every
  // late demote has been inserted, together with application helper queries.
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '      NIR_PASS(_, nir, nir_lower_helper_writes, true);\n      NIR_PASS(_, nir, nir_lower_is_helper_invocation);',
    '      NIR_PASS(_, nir, nir_lower_helper_writes, true);'));
  edits.set('src/kosmickrisp/compiler/kk_helper_side_effects.h',
    fs.readFileSync(new URL('./kosmickrisp-helper-side-effects.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('bool\nmsl_optimize_nir(struct nir_shader *nir)',
      '#include "kk_helper_side_effects.h"\n\nbool\nmsl_optimize_nir(struct nir_shader *nir)')
    .replace('   NIR_PASS(_, nir, nir_opt_shrink_stores, false);',
      '   NIR_PASS(_, nir, kk_nir_lower_helper_side_effects);\n   NIR_PASS(_, nir, nir_opt_shrink_stores, false);'));
}
