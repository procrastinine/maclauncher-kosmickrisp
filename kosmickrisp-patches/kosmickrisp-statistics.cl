/* SPDX-License-Identifier: MIT */

/* One invocation, ordered with the application dispatch by the ordinary
 * precompiled-kernel barriers. Consume the same predicated indirect arguments
 * as the real dispatch. Internal helper kernels never call this entry point.
 */
KERNEL(1)
libkk_count_compute(global uint64_t *counter, constant uint32_t *indirect,
                    uint32_t x, uint32_t y, uint32_t z,
                    uint32_t local_x, uint32_t local_y, uint32_t local_z)
{
   if (indirect) {
      x = indirect[0];
      y = indirect[1];
      z = indirect[2];
   }

   /* Both the product and the accumulated result use the query's full width. */
   uint64_t invocations = (uint64_t)x * y * z * local_x * local_y * local_z;
   *counter += invocations;
}

/* Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 * IA kernels adapted from pinned Honeykrisp/Poly with full-width reports.
 */
#include "poly/geometry.h"
#include "poly/prim.h"
#include "poly/tessellator.h"

static void
kk_increment_counters(global uint64_t *a, global uint64_t *b, global uint64_t *c, uint64_t count)
{
   if (a) *a += count;
   if (b) *b += count;
   if (c) *c += count;
}

KERNEL(1)
libkk_increment_ia(global uint64_t *ia_vertices,
                    global uint64_t *ia_primitives,
                    global uint64_t *vs_invocations, global uint64_t *c_prims,
                    global uint64_t *c_invs, constant uint32_t *draw,
                    enum mesa_prim prim, unsigned verts_per_patch, uint views)
{
   kk_increment_counters(ia_vertices, vs_invocations, NULL, (uint64_t)draw[0] * draw[1] * views);
   uint64_t prims = (uint64_t)poly_decomposed_prims_for_vertices_with_tess(prim, draw[0], verts_per_patch) * draw[1] * views;
   kk_increment_counters(ia_primitives, c_prims, c_invs, prims);
}

KERNEL(1024)
libkk_increment_ia_restart(global uint64_t *ia_vertices,
                            global uint64_t *ia_primitives,
                            global uint64_t *vs_invocations,
                            global uint64_t *c_prims, global uint64_t *c_invs,
                            constant uint32_t *draw, uint64_t index_buffer,
                            uint32_t index_buffer_range_el,
                            uint32_t restart_index, uint32_t index_size_B,
                            enum mesa_prim prim, unsigned verts_per_patch, uint views)
{
   uint tid = cl_global_id.x;
   unsigned count = draw[0];
   local uint scratch;

   uint start = draw[2];
   uint partial = 0;

   /* Count non-restart indices */
   for (uint i = tid; i < count; i += 1024) {
      uint index = poly_load_index(index_buffer, index_buffer_range_el,
                                   start + i, index_size_B);

      if (index != restart_index)
         partial++;
   }

   /* Accumulate the partials across the workgroup */
   scratch = 0;
   barrier(CLK_LOCAL_MEM_FENCE);
   atomic_add(&scratch, partial);
   barrier(CLK_LOCAL_MEM_FENCE);

   /* Elect a single thread from the workgroup to increment the counters */
   if (tid == 0) {
      kk_increment_counters(ia_vertices, vs_invocations, NULL,
                              (uint64_t)scratch * draw[1] * views);
   }

   /* TODO: We should vectorize this */
   if ((ia_primitives || c_prims || c_invs) && tid == 0) {
      uint accum = 0;
      int last_restart = -1;
      for (uint i = 0; i < count; ++i) {
         uint index = poly_load_index(index_buffer, index_buffer_range_el,
                                      start + i, index_size_B);

         if (index == restart_index) {
            accum += poly_decomposed_prims_for_vertices_with_tess(
               prim, i - last_restart - 1, verts_per_patch);
            last_restart = i;
         }
      }

      {
         accum += poly_decomposed_prims_for_vertices_with_tess(
            prim, count - last_restart - 1, verts_per_patch);
      }

      kk_increment_counters(ia_primitives, c_prims, c_invs, (uint64_t)accum * draw[1] * views);
   }
}


KERNEL(1)
libkk_accumulate_geometry_statistics(constant uint32_t *gi, constant uint32_t *gp,
                                     constant uint32_t *ci, constant uint32_t *cp,
                                     global uint64_t *out_gi, global uint64_t *out_gp,
                                     global uint64_t *out_ci, global uint64_t *out_cp,
                                     uint views)
{
   if (out_gi) *out_gi += (uint64_t)*gi * views;
   if (out_gp) *out_gp += (uint64_t)*gp * views;
   if (out_ci) *out_ci += (uint64_t)*ci * views;
   if (out_cp) *out_cp += (uint64_t)*cp * views;
}

KERNEL(1)
libkk_accumulate_tess_statistics(constant struct poly_tess_params *p,
                                 global uint64_t *tcs, global uint64_t *tes,
                                 global uint64_t *ci, global uint64_t *cp,
                                 uint views)
{
   if (tcs) *tcs += (uint64_t)p->nr_patches * views;
   if (tes) *tes += (uint64_t)*p->statistic * views;
   uint total = p->nr_patches ? p->counts[p->nr_patches - 1] : 0;
   uint prims = p->points_mode ? total : p->isolines ? total / 2 : total / 3;
   kk_increment_counters(ci, cp, NULL, (uint64_t)prims * views);
}
