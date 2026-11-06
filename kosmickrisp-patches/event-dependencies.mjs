// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const id = 'event-dependencies';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-event-dependencies.h'];
export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_event.c', (source, replace) =>
    replace('   /* We do nothing, event should already be set by the time we are here. */',
      '   kk_event_wait_dependencies(commandBuffer, eventCount, pDependencyInfos);')
      .replace('#include "kk_entrypoints.h"', '#include "kk_entrypoints.h"\n#include "kk_event_dependencies.h"'));
  edits.set('src/kosmickrisp/vulkan/kk_event_dependencies.h',
    fs.readFileSync(new URL('./kosmickrisp-event-dependencies.h', import.meta.url), 'utf8'));
}
