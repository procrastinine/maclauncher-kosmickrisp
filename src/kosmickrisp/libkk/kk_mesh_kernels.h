/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_KERNELS_H
#define KK_MESH_KERNELS_H
#include "kk_mesh_dispatch.h"

static inline bool
kk_mesh_draw_enabled(KK_MESH_GLOBAL const struct kk_mesh_params *p)
{
   if (p->indirect_count && p->draw_id >=
       *(KK_MESH_GLOBAL const uint32_t *)p->indirect_count)
      return false;
   return !p->predicate ||
      ((*(KK_MESH_GLOBAL const uint32_t *)p->predicate != 0) !=
       (p->predicate_inverted != 0));
}

static inline void
kk_mesh_filter_grid(KK_MESH_GLOBAL struct kk_mesh_params *p)
{
   KK_MESH_GLOBAL struct kk_mesh_batch *b =
      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;
   KK_MESH_GLOBAL const uint32_t *g =
      (KK_MESH_GLOBAL const uint32_t *)p->indirect_grid;
   if (!kk_mesh_draw_enabled(p)) {
      b->mesh_dispatch[0] = 0;
      b->mesh_dispatch[1] = b->mesh_dispatch[2] = 1;
      return;
   }
   b->mesh_dispatch[0] = g[0];
   b->mesh_dispatch[1] = g[1];
   b->mesh_dispatch[2] = g[2];
}

/* One record per logical indirect draw, captured before its render pass.
 * Disabled count/predicate paths never dereference the grid address. */
static inline void
kk_mesh_snapshot_grid(KK_MESH_GLOBAL const struct kk_mesh_params *p,
   KK_MESH_GLOBAL uint32_t *out)
{
   bool predicate = !p->predicate ||
      ((*(KK_MESH_GLOBAL const uint32_t *)p->predicate != 0) !=
       (p->predicate_inverted != 0));
   uint32_t count = predicate && p->max_draw_count ? (p->indirect_count ?
      *(KK_MESH_GLOBAL const uint32_t *)p->indirect_count : p->max_draw_count) : 0;
   out[3] = count < p->max_draw_count ? count : p->max_draw_count;
   bool active = p->draw_id < out[3];
   KK_MESH_GLOBAL const uint32_t *grid =
      (KK_MESH_GLOBAL const uint32_t *)p->indirect_grid;
   out[0] = active ? grid[0] : 0;
   out[1] = active ? grid[1] : 1;
   out[2] = active ? grid[2] : 1;
}

/* KK_MESH_GLOBAL is global for OpenCL and device for MSL. */
static inline void
kk_mesh_prepare_batch(KK_MESH_GLOBAL struct kk_mesh_params *p)
{
   KK_MESH_GLOBAL struct kk_mesh_batch *b =
      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;
   KK_MESH_GLOBAL const uint32_t *grid =
      (KK_MESH_GLOBAL const uint32_t *)p->indirect_grid;
   bool enabled = kk_mesh_draw_enabled(p);
   uint32_t x = enabled ? grid[0] : 0;
   uint32_t y = enabled ? grid[1] : 1;
   uint32_t z = enabled ? grid[2] : 1;
   uint64_t total = (uint64_t)x * y * z;
   uint32_t count = 0;
   if (total > p->task_base) {
      uint64_t left = total - p->task_base;
      uint32_t limit = p->pad ? p->max_draw_count : KK_MESH_BATCH_SIZE;
      count = left > limit ? limit : (uint32_t)left;
   }
   b->task_dispatch[0] = count;
   b->task_dispatch[1] = b->task_dispatch[2] = 1;
   b->task_count = count;
   b->mesh_dispatch[0] = 0;
   b->mesh_dispatch[1] = b->mesh_dispatch[2] = 1;
   b->mesh_count = 0;
   b->input_grid[0] = x;
   b->input_grid[1] = y;
   b->input_grid[2] = z;
   KK_MESH_GLOBAL struct kk_mesh_task_record *records =
      (KK_MESH_GLOBAL struct kk_mesh_task_record *)p->records;
   for (uint32_t i = 0; i < count; ++i) {
      records[i].grid[0] = records[i].grid[1] = records[i].grid[2] = 0;
      records[i].prefix_end = 0;
   }
}

/* The maximum batch sum, 256 * 2^22, fits uint32_t and the 2-D physical grid.
 * Inclusive ends also handle task groups emitting zero mesh groups. */
static inline void
kk_mesh_prefix_batch(KK_MESH_GLOBAL struct kk_mesh_params *p)
{
   KK_MESH_GLOBAL struct kk_mesh_batch *b =
      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;
   KK_MESH_GLOBAL struct kk_mesh_task_record *r =
      (KK_MESH_GLOBAL struct kk_mesh_task_record *)p->records;
   uint32_t sum = 0;
   for (uint32_t i = 0; i < b->task_count; ++i) {
      sum += r[i].grid[0] * r[i].grid[1] * r[i].grid[2];
      r[i].prefix_end = sum;
   }
   b->mesh_count = sum;
   uint32_t width = sum < KK_MESH_MAX_DIMENSION ? sum : KK_MESH_MAX_DIMENSION;
   b->mesh_dispatch[0] = width;
   b->mesh_dispatch[1] = width ? (sum + width - 1) / width : 1;
   b->mesh_dispatch[2] = 1;
}

static inline void
kk_mesh_slice_grid(KK_MESH_GLOBAL uint32_t *grid, uint32_t count)
{
   uint32_t width = count < KK_MESH_MAX_DIMENSION ? count : KK_MESH_MAX_DIMENSION;
   uint32_t height = width ? (count + width - 1) / width : 1;
   if ((uint64_t)width * height > KK_MESH_MAX_PHYSICAL_GROUPS) {
      width = count < 32768u ? count : 32768u;
      height = width ? (count + width - 1) / width : 1;
   }
   grid[0] = width;
   grid[1] = height;
   grid[2] = 1;
}

static inline void
kk_mesh_prefix_slices(KK_MESH_GLOBAL struct kk_mesh_params *p,
                        KK_MESH_GLOBAL uint32_t *slices, uint32_t slice_count)
{
   kk_mesh_prefix_batch(p);
   KK_MESH_GLOBAL struct kk_mesh_batch *b =
      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;
   uint32_t span = p->pad ? p->pad : KK_MESH_MAX_PHYSICAL_GROUPS;
   for (uint32_t i = 0; i < slice_count; ++i) {
      uint32_t base = i * span;
      uint32_t left = b->mesh_count > base ? b->mesh_count - base : 0;
      uint32_t count = left < span ? left : span;
      kk_mesh_slice_grid(slices + 3u * i, count);
   }
   for (uint32_t i = 0; i < 3; ++i)
      b->mesh_dispatch[i] = slices[i];
}

static inline void
kk_mesh_filter_slices(KK_MESH_GLOBAL struct kk_mesh_params *p,
                        KK_MESH_GLOBAL uint32_t *slices, uint32_t slice_count)
{
   KK_MESH_GLOBAL struct kk_mesh_batch *b =
      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;
   KK_MESH_GLOBAL const uint32_t *grid =
      (KK_MESH_GLOBAL const uint32_t *)p->indirect_grid;
   bool enabled = kk_mesh_draw_enabled(p);
   uint32_t x = enabled ? grid[0] : 0;
   uint32_t y = enabled ? grid[1] : 1;
   uint32_t z = enabled ? grid[2] : 1;
   uint32_t total = (uint64_t)x * y * z;
   b->input_grid[0] = x;
   b->input_grid[1] = y;
   b->input_grid[2] = z;
   for (uint32_t i = 0; i < slice_count; ++i) {
      uint32_t base = p->task_base + i * p->payload_stride;
      uint32_t left = total > base ? total - base : 0;
      uint32_t count = left < p->payload_stride ? left : p->payload_stride;
      if (base == 0 && count == total) {
         slices[0] = x;
         slices[1] = y;
         slices[2] = z;
      } else {
         kk_mesh_slice_grid(slices + 3u * i, count);
      }
   }
}

/* Prefix and expand only real output; unused capacity is never traversed. */
static inline void
kk_mesh_prefix_expand(KK_MESH_GLOBAL struct kk_mesh_params *p,
   KK_MESH_GLOBAL uint32_t *grids, KK_MESH_GLOBAL uint32_t *range,
   uint32_t span, uint32_t capacity)
{
   kk_mesh_prefix_batch(p);
   KK_MESH_GLOBAL struct kk_mesh_batch *batch =
      (KK_MESH_GLOBAL struct kk_mesh_batch *)p->batch;
   uint32_t total = batch->mesh_count;
   uint32_t needed = total / span + (total % span != 0);
   range[0] = 0;
   range[1] = needed;
   batch->pad = needed > 1;
   if (!needed) grids[0] = 0, grids[1] = grids[2] = 1;
   for (uint32_t i = 0; i < needed && i < capacity; ++i) {
      uint32_t left = total - i * span;
      kk_mesh_slice_grid(grids + 3u * i, left < span ? left : span);
   }
}

#endif
