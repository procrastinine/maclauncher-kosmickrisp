// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'native-block-alias';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-native-block-alias.h', 'kosmickrisp-patches/kosmickrisp-native-block-alias.c'];
export function apply({edit, edits}) {
  for (const [source, target] of [['kosmickrisp-native-block-alias.h', 'kk_native_block_alias.h'], ['kosmickrisp-native-block-alias.c', 'kk_native_block_alias.c']])
    edits.set('src/kosmickrisp/vulkan/' + target, fs.readFileSync(new URL('./' + source, import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/meson.build', s => s.replace("  'kk_image_view.c',", "  'kk_image_view.c',\n  'kk_native_block_alias.c',"));
}
