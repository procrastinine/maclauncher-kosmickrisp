// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'tessellation-indirect-grids';
export const sourceFiles = ['kosmickrisp-patches/tessellation-indirect-grid.h'];
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/libkk/kk_tess_grid.h',
    fs.readFileSync(new URL('./tessellation-indirect-grid.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/libkk/kk_tessellation.cl', s => s
    .replace('#include "poly/tessellator.h"', '#include "poly/tessellator.h"\n#include "kk_tess_grid.h"')
    .replace('   global uint32_t *grids /* output: VS then TCS then tess */,',
      '   global struct kk_tess_grid *grids /* VS, TCS, tess */,')
    .replace('   global uint32_t *tcs_statistic)',
      '   global uint32_t *tcs_statistic, uint32_t vs_group_size)')
    .replace(`   /* VS grid size */
   grids[0] = count;
   grids[1] = instance_count;
   grids[2] = 1;

   /* TCS grid size */
   grids[3] = in_patches * p->output_patch_size;
   grids[4] = instance_count;
   grids[5] = 1;

   /* Tess grid size */
   grids[6] = unrolled_patches;
   grids[7] = 1;
   grids[8] = 1;`,
`   grids[0] = (struct kk_tess_grid){
      {count, instance_count, 1}, {vs_group_size, 1, 1}};
   grids[1] = (struct kk_tess_grid){
      {in_patches * p->output_patch_size, instance_count, 1},
      {p->output_patch_size, 1, 1}};
   grids[2] = (struct kk_tess_grid){
      {unrolled_patches, 1, 1}, {1, 1, 1}};`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kosmickrisp/libkk/kk_tess_grid.h"')
    .replaceAll('uint32_t grid_stride = sizeof(uint32_t) * 3;',
      'uint32_t grid_stride = sizeof(struct kk_tess_grid);')
    .replaceAll('kk_grid_indirect(gfx->tess.indirect_ptr.gpu +',
      'kk_grid_indirect_local(gfx->tess.indirect_ptr.gpu +')
    .replace('         .tcs_statistic = 0,',
      '         .tcs_statistic = 0,\n         .vs_group_size = KK_POLY_WG_SIZE,'));
}
