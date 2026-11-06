// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export function adaptSoftFp64Library(original) {
  const replacements = [
    ['increment = zFrac2 < 0u;', 'increment = int(zFrac2) < 0;'],
    ['zFrac1 &= ~((zFrac2 + uint(zFrac2 == 0u)) & uint(roundNearestEven));',
      'zFrac1 &= ~uint(roundNearestEven && zFrac2 == 0x80000000u);'],
  ];
  let source = String(original);
  for (const [before, after] of replacements) {
    if (!source.includes(before)) throw Error('Soft-FP64 library anchor missing: ' + before);
    source = source.replace(before, after);
  }
  source = source.replace('return packUint2x32(uvec2(zLo, zHi));',
    'return packUint2x32(uvec2(zLo, zHi | (a.y & 0x80000000u)));');
  const start = source.indexOf('__fround64(uint64_t __a)');
  const open = source.indexOf('{', start);
  let end = open + 1, depth = 1;
  while (depth && end < source.length) {
    if (source[end] === '{') depth++;
    if (source[end] === '}') depth--;
    end++;
  }
  if (start < 0 || depth) throw Error('Soft-FP64 round function anchor missing');
  source = source.slice(0, start - 'uint64_t\n'.length) +
    'uint64_t kk_f64_round_even(uint64_t a);\nuint64_t\n' + source.slice(start, open) +
    '{ return kk_f64_round_even(__a); }' + source.slice(end);
  return source + '\n' + fs.readFileSync(new URL('./kosmickrisp-soft-fp64-extra.glsl', import.meta.url), 'utf8');
}
