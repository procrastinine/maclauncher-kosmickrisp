// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

// Apple's maximumDrawableCount range is two to three; MAILBOX requires
// replacement of pending presents, which this FIFO/IMMEDIATE path does not do.
export const id = 'metal-drawable-count';
export function apply({base: sourceRoot, edits}) {
  const file='src/vulkan/wsi/wsi_common_metal.c';
  let source=edits.get(file) ?? fs.readFileSync(path.join(sourceRoot,file),'utf8');
  const r=(from,to)=>{if(!source.includes(from))throw Error('Missing Metal image-count anchor');source=source.replace(from,to);};
    r(`   /* Force recommended 3 drawables, otherwise the OS frame pacing may slow
    * down presentation.
    */
   caps->surfaceCapabilities.minImageCount = 3;`, `   /* CAMetalLayer supports double and triple buffering. */
   caps->surfaceCapabilities.minImageCount = 2;`);
  edits.set(file,source);
}
