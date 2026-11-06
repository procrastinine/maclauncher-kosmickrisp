// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'sampler-parameters';
export const sourceFiles = ['kk-sampler-params.h', 'kk-sampler-params-init.h']
  .map(name => 'kosmickrisp-patches/' + name);
export function apply({edits}) {
  edits.set('src/kosmickrisp/compiler/kk-sampler-params.h',
    fs.readFileSync(new URL('kk-sampler-params.h', import.meta.url), 'utf8'));
  edits.set('src/kosmickrisp/vulkan/kk-sampler-params-init.h',
    fs.readFileSync(new URL('kk-sampler-params-init.h', import.meta.url), 'utf8'));
}
