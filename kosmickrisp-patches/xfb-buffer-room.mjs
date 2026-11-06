// SPDX-License-Identifier: MIT
export function apply({edit}) {
edit('src/poly/cl/geometry.cl', s => s
    .replace(`   int numer_s = p->xfb_size[i] + (stride - max_output_end) - xfb_offset;
   uint numer = max(numer_s, 0);
   return numer / (stride * vertices_per_prim);`,
      `   uint64_t remaining = p->xfb_size[i] > xfb_offset
      ? (uint64_t)p->xfb_size[i] - xfb_offset : 0;
   if (remaining < max_output_end)
      return 0;
   return (remaining + stride - max_output_end) /
          ((uint64_t)stride * vertices_per_prim);`));
}
