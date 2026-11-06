// SPDX-License-Identifier: MIT
export const id = 'border-interpolation';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_nir_lower_custom_border.c', (s, replace) => replace(
    '         replaced = nir_flrp(b, clamp_to_0, clamp_to_1, border);',
    `         replaced = nir_flrp(b, clamp_to_0, clamp_to_1, border);
         /* Identical samples have no border contribution. Preserve their
          * exact bits, avoiding inf-inf and non-finite border contamination. */
         replaced = nir_bcsel(b, nir_ieq(b, clamp_to_0, clamp_to_1),
                              clamp_to_0, replaced);`));
}
