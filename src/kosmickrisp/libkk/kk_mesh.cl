/* SPDX-License-Identifier: MIT */
#include "compiler/libcl/libcl.h"
#include "kk_mesh_types.h"
#include "poly/geometry.h"
#define KK_MESH_GLOBAL global
#include "kk_mesh_kernels.h"
#undef KK_MESH_GLOBAL

/* A preceding render encoder's completion barrier protects every fragment
 * reading the previous packet batch. Preserve any DGC preprocess reservation. */
KERNEL(1)
libkk_mesh_reset_heap(global struct poly_heap *heap, constant uint32_t *floor)
{
   heap->bottom = floor ? *floor : 0;
}

KERNEL(1)
libkk_mesh_filter(global struct kk_mesh_params *p)
{
   kk_mesh_filter_grid(p);
}

KERNEL(1)
libkk_mesh_prepare(global struct kk_mesh_params *p)
{
   kk_mesh_prepare_batch(p);
}

/* A batch has only 256 records. A single GPU lane's ordered scan avoids
 * auxiliary allocations and inter-workgroup synchronization. The worst sum
 * is 256 * 2^22, which fits uint32_t and a two-dimensional Metal grid. */
KERNEL(1)
libkk_mesh_prefix(global struct kk_mesh_params *p)
{
   kk_mesh_prefix_batch(p);
}

KERNEL(1)
libkk_mesh_prefix_slices(global struct kk_mesh_params *p,
                          global uint32_t *slices, uint32_t slice_count)
{
   kk_mesh_prefix_slices(p, slices, slice_count);
}

KERNEL(1)
libkk_mesh_filter_slices(global struct kk_mesh_params *p,
                          global uint32_t *slices, uint32_t slice_count)
{
   kk_mesh_filter_slices(p, slices, slice_count);
}

/* Snapshot in queue order, after preceding producers and before this draw.
 * The host continuation consumes only this fixed four-word packet. */
KERNEL(1)
libkk_mesh_snapshot(global struct kk_mesh_params *p, global uint32_t *out)
{
   uint32_t count = p->indirect_count ? *(global uint32_t *)p->indirect_count : p->max_draw_count;
   out[3] = min(count, p->max_draw_count);
   global uint32_t *grid = (global uint32_t *)p->indirect_grid;
   bool active = p->draw_id < out[3] && kk_mesh_draw_enabled(p);
   out[0] = active ? grid[0] : 0;
   out[1] = active ? grid[1] : 1;
   out[2] = active ? grid[2] : 1;
}

KERNEL(1)
libkk_mesh_prefix_expand(global struct kk_mesh_params *p,
   global uint32_t *grids, global uint32_t *range, uint32_t span, uint32_t capacity)
{
   kk_mesh_prefix_expand(p, grids, range, span, capacity);
}
