// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

// VkPresentRegionsKHR supplies optional damage hints.  Mesa already passes
// each region to Metal WSI, whose full-image blit/present is valid for all hints.
export const id = 'incremental-present';
export function apply({base: sourceRoot, edits}) {
  const file = 'src/kosmickrisp/vulkan/kk_physical_device.c';
  const source = edits.get(file) ?? fs.readFileSync(path.join(sourceRoot, file), 'utf8');
  const anchor = '      .KHR_swapchain = true,';
  if (!source.includes(anchor)) throw Error('Missing swapchain extension anchor');
  edits.set(file, source.replace(anchor, anchor + '\n      .KHR_incremental_present = true,'));
}
