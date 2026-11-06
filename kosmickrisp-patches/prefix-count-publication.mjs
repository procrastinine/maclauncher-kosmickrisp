// SPDX-License-Identifier: MIT
export const id = 'prefix-count-publication';
export function apply({edit}) {
  // The scan writes device memory in other lanes before lane zero reads its total.
  edit('src/kosmickrisp/libkk/kk_geometry.cl', s => s.replace(
    '   barrier(CLK_LOCAL_MEM_FENCE);\n   if (cl_local_id.x != 0)',
    '   barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);\n   if (cl_local_id.x != 0)'));
}
