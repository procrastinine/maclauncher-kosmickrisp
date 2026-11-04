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

   unsigned unrolled_patches = in_patches * instance_count;

   /* Sizes are computed in 64 bits so a large draw fails instead of wrapping
    * to a small allocation that the dispatches overrun.
    */
   const ulong unrolled = (ulong)in_patches * instance_count;
   ulong alloc = 0;
   ulong tcs_out_offs = alloc;
   alloc += poly_heap_bytes(unrolled, p->tcs_stride_el * 4);

   ulong patch_coord_offs = alloc;
   alloc += poly_heap_bytes(unrolled, 4);

   ulong count_offs = alloc;
   alloc += poly_heap_bytes(unrolled, sizeof(uint32_t));

   ulong vb_offs = alloc;
   alloc += poly_heap_bytes((ulong)count * instance_count,
                            util_bitcount64(vertex_outputs) * 16);

   /* Allocate all patch calculations in one go */
   uint64_t blob_addr = poly_heap_try_alloc(p->heap, alloc, 16);

   /* The grids are reused pool memory that sizes the VS, TCS and tessellator
    * dispatches, and the prefix sum reads nr_patches: on overflow, run
    * nothing.
    */
   if (blob_addr == POLY_HEAP_PTR_FAILED) {
      p->nr_patches = 0;
      grids[0] = (struct kk_tess_grid){{0, 0, 0}, {vs_group_size, 1, 1}};
      grids[1] = (struct kk_tess_grid){{0, 0, 0}, {p->output_patch_size, 1, 1}};
      grids[2] = (struct kk_tess_grid){{0, 0, 0}, {1, 1, 1}};
      return;
   }
   global uchar *blob = (global uchar *)blob_addr;

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
