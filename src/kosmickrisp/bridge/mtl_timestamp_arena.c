/* SPDX-License-Identifier: MIT */
#include "mtl_timestamp_arena.h"

#include "mtl_bridge.h"
#include "mtl_device.h"

#include "util/macros.h"
#include "util/simple_mtx.h"

#include <stdlib.h>

#define MTL_TIMESTAMP_ARENA_HEAP_ENTRIES 4096

struct mtl_timestamp_arena {
   mtl_device *device;
   simple_mtx_t lock;
   uint32_t unit_entries, max_heaps;
   /* Fixed by the first heap; zero before it. A unit u lives in heap u / units_per_heap. */
   uint32_t units_per_heap;
   uint32_t heap_count, free_count;
   bool retry_heap, heap_failed;
   /* Room for every unit of max_heaps heaps, allocated with the first heap. */
   uint32_t *free_units;
   mtl_counter_heap *heaps[];
};

struct mtl_timestamp_arena *
mtl_timestamp_arena_create(mtl_device *dev, uint32_t unit_entries,
                           uint32_t max_heaps, bool retry_heap)
{
   if (!unit_entries || unit_entries > MTL_TIMESTAMP_ARENA_HEAP_ENTRIES ||
       !max_heaps)
      return NULL;

   struct mtl_timestamp_arena *arena =
      calloc(1, sizeof(*arena) + (size_t)max_heaps * sizeof(arena->heaps[0]));
   if (!arena)
      return NULL;

   arena->device = dev;
   arena->unit_entries = unit_entries;
   arena->max_heaps = max_heaps;
   arena->retry_heap = retry_heap;
   simple_mtx_init(&arena->lock, mtx_plain);
   return arena;
}

void
mtl_timestamp_arena_destroy(struct mtl_timestamp_arena *arena)
{
   if (!arena)
      return;

   for (uint32_t i = 0; i < arena->heap_count; ++i)
      mtl_release(arena->heaps[i]);
   free(arena->free_units);
   simple_mtx_destroy(&arena->lock);
   free(arena);
}

/* With the lock held: adds one heap's units to the free list. */
static bool
add_heap(struct mtl_timestamp_arena *arena)
{
   if (arena->heap_count == arena->max_heaps ||
       (arena->heap_failed && !arena->retry_heap))
      return false;

   const uint32_t unit = arena->unit_entries;
   uint32_t entries = arena->units_per_heap
                         ? arena->units_per_heap * unit
                         : MTL_TIMESTAMP_ARENA_HEAP_ENTRIES / unit * unit;
   mtl_counter_heap *heap = mtl_new_timestamp_counter_heap(arena->device, entries);

   /* Only the first heap settles the size, halving while Metal refuses it. */
   while (!heap && !arena->units_per_heap && entries / 2 >= unit) {
      entries = entries / 2 / unit * unit;
      heap = mtl_new_timestamp_counter_heap(arena->device, entries);
   }
   if (heap && mtl_timestamp_counter_heap_count(heap) < entries) {
      mtl_release(heap);
      heap = NULL;
   }

   if (heap && !arena->units_per_heap) {
      arena->free_units = malloc((size_t)arena->max_heaps * (entries / unit) *
                                 sizeof(*arena->free_units));
      if (arena->free_units) {
         arena->units_per_heap = entries / unit;
      } else {
         mtl_release(heap);
         heap = NULL;
      }
   }

   if (!heap) {
      arena->heap_failed = true;
      return false;
   }

   const uint32_t base = arena->heap_count * arena->units_per_heap;
   arena->heaps[arena->heap_count++] = heap;
   for (uint32_t i = arena->units_per_heap; i--;)
      arena->free_units[arena->free_count++] = base + i;
   return true;
}

bool
mtl_timestamp_arena_take(struct mtl_timestamp_arena *arena, uint32_t count,
                         uint32_t *units)
{
   if (!arena)
      return false;

   simple_mtx_lock(&arena->lock);
   /* A request beyond every heap the arena may still add fails without adding them. */
   const bool fits =
      !arena->units_per_heap ||
      count - MIN2(count, arena->free_count) <=
         (uint64_t)(arena->max_heaps - arena->heap_count) * arena->units_per_heap;
   while (fits && arena->free_count < count && add_heap(arena))
      ;
   const bool taken = arena->free_count >= count;
   if (taken) {
      for (uint32_t i = 0; i < count; ++i)
         units[i] = arena->free_units[--arena->free_count];
   }
   simple_mtx_unlock(&arena->lock);
   return taken;
}

void
mtl_timestamp_arena_give(struct mtl_timestamp_arena *arena, uint32_t count,
                         const uint32_t *units)
{
   if (!count)
      return;

   simple_mtx_lock(&arena->lock);
   for (uint32_t i = count; i--;)
      arena->free_units[arena->free_count++] = units[i];
   simple_mtx_unlock(&arena->lock);
}

mtl_counter_heap *
mtl_timestamp_arena_unit(const struct mtl_timestamp_arena *arena, uint32_t unit,
                         uint32_t *heap_index, uint32_t *first_entry)
{
   const uint32_t heap = unit / arena->units_per_heap;
   if (heap_index)
      *heap_index = heap;
   *first_entry = unit % arena->units_per_heap * arena->unit_entries;
   return arena->heaps[heap];
}
