// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/kosmickrisp/vulkan/kk_debug.c', s => s.replace('experimental_flags, 0);', 'experimental_flags, KK_EXPERIMENTAL_CUSTOM_BORDER);'));
}
