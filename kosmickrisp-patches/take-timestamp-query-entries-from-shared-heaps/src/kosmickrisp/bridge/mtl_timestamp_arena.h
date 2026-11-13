/* SPDX-License-Identifier: MIT */
#ifndef MTL_TIMESTAMP_ARENA_H
#define MTL_TIMESTAMP_ARENA_H 1

#include "mtl_types.h"

#include <stdbool.h>
#include <stdint.h>

/* Timestamp counter heap entries shared by many owners.
 *
 * AGX allows a process 32 live timestamp counter heaps, and Metal rejects heaps over 4096 entries,
 * so one heap per owner (a query pool, a timed encoder) runs out long before the entries do. An
 * arena hands out units of unit_entries consecutive entries from heaps of one size, creating a
 * heap when the free units run out and keeping it until the arena is destroyed. Take and give
 * hold the arena's lock and never wait on the GPU; a held unit's place is read without it. */
struct mtl_timestamp_arena;

/* At most max_heaps heaps. After Metal refuses a heap, a later take tries again only with
 * retry_heap. */
struct mtl_timestamp_arena *mtl_timestamp_arena_create(mtl_device *dev,
                                                       uint32_t unit_entries,
                                                       uint32_t max_heaps,
                                                       bool retry_heap);
void mtl_timestamp_arena_destroy(struct mtl_timestamp_arena *arena);

/* Takes count units into units and returns true, or takes none and returns false. */
bool mtl_timestamp_arena_take(struct mtl_timestamp_arena *arena, uint32_t count,
                              uint32_t *units);
void mtl_timestamp_arena_give(struct mtl_timestamp_arena *arena, uint32_t count,
                              const uint32_t *units);

/* The heap of a held unit, the heap's index in the arena and the unit's first entry in it. */
mtl_counter_heap *mtl_timestamp_arena_unit(const struct mtl_timestamp_arena *arena,
                                           uint32_t unit, uint32_t *heap_index,
                                           uint32_t *first_entry);

#endif /* MTL_TIMESTAMP_ARENA_H */
