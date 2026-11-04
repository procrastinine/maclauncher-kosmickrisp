/*
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * Copyright 2023 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "poly/geometry.h"
#include "poly/tessellator.h"
#include "kk_tess_grid.h"

KERNEL(1)
libkk_tess_setup_indirect(
   global struct poly_tess_params *p,
   global struct kk_tess_grid *grids /* VS, TCS, tess */,
   global struct poly_vertex_params *vp /* output */, global uint32_t *indirect,
   uint64_t in_index_buffer, uint32_t in_index_buffer_range_el,
   uint32_t in_index_size_B, uint64_t vertex_outputs /* bitfield */,

   /* Tess control invocation counter if active, else zero */
   global uint32_t *tcs_statistic, uint32_t vs_group_size)
{
   uint count = indirect[0], instance_count = indirect[1];
   unsigned in_patches = count / p->input_patch_size;

   /* TCS invocation counter increments once per-patch */
   if (tcs_statistic) {
      *tcs_statistic += in_patches;
   }

   size_t draw_stride = 5 * sizeof(uint32_t);
   unsigned unrolled_patches = in_patches * instance_count;

   uint32_t alloc = 0;
   uint32_t tcs_out_offs = alloc;
   alloc += unrolled_patches * p->tcs_stride_el * 4;

   uint32_t patch_coord_offs = alloc;
   alloc += unrolled_patches * 4;

   uint32_t count_offs = alloc;
   alloc += unrolled_patches * sizeof(uint32_t);

   uint vb_offs = alloc;
   uint vb_size = poly_tcs_in_size(count * instance_count, vertex_outputs);
   alloc += vb_size;

   /* Allocate all patch calculations in one go */
   global uchar *blob = poly_heap_alloc(p->heap, alloc, 16);

   p->tcs_buffer = (global float *)(blob + tcs_out_offs);
   p->patches_per_instance = in_patches;
   p->coord_allocs = (global uint *)(blob + patch_coord_offs);
   p->nr_patches = unrolled_patches;

   vp->output_buffer = (uintptr_t)(blob + vb_offs);
   vp->outputs = vertex_outputs;
   p->counts = (global uint32_t *)(blob + count_offs);

   if (vp) {
      vp->verts_per_instance = count;
   }

   /* If indexing is enabled, the third word is the offset into the index buffer
    * in elements. Apply that offset now that we have it. For a hardware
    * indirect draw, the hardware would do this for us, but for software input
    * assembly we need to do it ourselves.
    *
    * XXX: Deduplicate?
    */
   if (in_index_size_B) {
      /* TODO_KOSMICKRISP Use poly_index_buffer and implement
       * load_ro_sink_address_poly */
      vp->index_buffer = in_index_buffer + (indirect[2] * in_index_size_B);

      vp->index_buffer_range_el =
         poly_index_buffer_range_el(in_index_buffer_range_el, indirect[2]);
   }

   grids[0] = (struct kk_tess_grid){
      {count, instance_count, 1}, {vs_group_size, 1, 1}};
   grids[1] = (struct kk_tess_grid){
      {in_patches * p->output_patch_size, instance_count, 1},
      {p->output_patch_size, 1, 1}};
   grids[2] = (struct kk_tess_grid){
      {unrolled_patches, 1, 1}, {1, 1, 1}};
}
