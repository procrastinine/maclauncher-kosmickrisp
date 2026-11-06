// SPDX-License-Identifier: MIT
export function apply({edit}) {
// These passes follow variant creation and must see that variant, just like
  // preprocessing and explicit-I/O lowering immediately around them.
  edit('src/kosmickrisp/clc/kk_clc.c', s => s
    .replace('msl_preprocess_nir_workarounds(nir, 0);', 'msl_preprocess_nir_workarounds(s, 0);')
    .replace('msl_optimize_nir(nir);', 'msl_optimize_nir(s);'));
}
