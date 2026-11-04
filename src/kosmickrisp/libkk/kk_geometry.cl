/*
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"
#include "poly/tessellator.h"

KERNEL(1024)
libkk_prefix_sum_tess(global struct poly_tess_params *p)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->counts, p->nr_patches, 1 /* words */,
                   0 /* word */, 1024);

   /* After prefix summing, we know the total # of indices, so allocate the
    * index buffer now. Elect a thread for the allocation.
    */
   barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);
   if (cl_local_id.x != 0)
      return;

   /* The last element of an inclusive prefix sum is the total sum */
   uint total = p->nr_patches > 0 ? p->counts[p->nr_patches - 1] : 0;
   global uint32_t *desc = p->out_draws;

   /* Allocate 4-byte indices */
   uint32_t elsize_B = sizeof(uint32_t);
   uint alloc_B = poly_heap_try_alloc_offs(p->heap, (ulong)total * elsize_B, 16);

   /* The draw buffer is reused pool memory: on overflow, draw nothing. */
   if (alloc_B == POLY_HEAP_ALLOC_FAILED) {
      desc[0] = 0;
      desc[1] = 0;
      return;
   }

   p->index_buffer = (global uint32_t *)(((uintptr_t)p->heap->base) + alloc_B);

   /* ...and now we can generate the API indexed draw */
   desc[0] = total;              /* count */
   desc[1] = 1;                  /* instance_count */
   desc[2] = alloc_B / elsize_B; /* start */
   desc[3] = 0;                  /* index_bias */
   desc[4] = 0;                  /* start_instance */
}

KERNEL(1)
libkk_gs_setup_indirect(uint64_t index_buffer, constant uint *draw,
                        global struct poly_vertex_params *vp,
                        global struct poly_geometry_params *p,
                        global struct poly_heap *heap, uint64_t vs_outputs,
                        uint32_t index_size_B, uint32_t index_buffer_range_el,
                        uint32_t prim, int is_prefix_summing, uint32_t max_indices,
                        enum poly_gs_shape shape)
{
   poly_gs_setup_indirect(index_buffer, draw, vp, p, heap, vs_outputs,
                          index_size_B, index_buffer_range_el, prim,
                          is_prefix_summing, max_indices, shape);
}

KERNEL(1024)
libkk_prefix_sum_geometry(global struct poly_geometry_params *p)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->count_buffer, p->input_primitives,
      p->count_buffer_stride / 4, cl_group_id.x, 1024);
}

KERNEL(1)
libkk_xfb_counter_copy(uint64_t source, uint64_t destination)
{
   *(global uint32_t *)destination = source ? *(global uint32_t *)source : 0;
}

KERNEL(1)
libkk_xfb_draw(constant uint32_t *counter, global uint32_t *draw,
              uint32_t instance_count, uint32_t first_instance,
              uint32_t counter_offset, uint32_t vertex_stride)
{
   draw[0] = max((int)*counter - (int)counter_offset, 0) / vertex_stride;
   draw[1] = instance_count;
   draw[2] = 0;
   draw[3] = first_instance;
}

/* Count the actual post-assembly draw when no application/synthetic GS runs.
 * The same two leading words describe direct/indexed/tessellated draws.
 * Predicates and topology rewrites have already been applied. */
KERNEL(1)
libkk_primitives_generated(global uint64_t *report, constant uint32_t *draw,
                           uint32_t vertices, uint32_t instances, uint32_t mode)
{
   if (draw) {
      vertices = draw[0];
      instances = draw[1];
   }
   *report += (uint64_t)u_decomposed_prims_for_vertices(mode, vertices) * instances;
}
