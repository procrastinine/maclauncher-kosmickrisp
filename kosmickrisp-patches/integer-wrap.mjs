// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'integer-wrap';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-integer-wrap.h'];
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/compiler/kk_integer_wrap.h',
    fs.readFileSync(new URL('./kosmickrisp-integer-wrap.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => {
    const include = '#include "kk_fp32_preserve.h"';
    const hook = '   if (kk_fp32_preserve_alu(ctx, instr)) return;';
    if (!s.includes(include) || !s.includes(hook)) throw Error('Integer wrap requires fp32-denorm-preserve emitter anchors');
    return s.replace(include, include + '\n#include "kk_integer_wrap.h"')
      .replace(hook, '   if (kk_msl_integer_wrap(ctx, instr)) return;\n' + hook);
  });
}
