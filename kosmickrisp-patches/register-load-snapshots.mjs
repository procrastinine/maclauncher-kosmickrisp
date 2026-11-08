// SPDX-License-Identifier: MIT
export const id = 'register-load-snapshots';
export function apply({edit}) {
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s.replace(
    '   NIR_PASS(_, nir, nir_trivialize_registers);\n   NIR_PASS(_, nir, nir_opt_copy_prop);',
    '   NIR_PASS(_, nir, nir_opt_copy_prop);\n   /* Register loads are inlined into their uses by the MSL emitter. Keep\n    * their snapshot copies after any pass that propagates moves. */\n   NIR_PASS(_, nir, nir_trivialize_registers);'));
}
