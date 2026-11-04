/* SPDX-License-Identifier: MIT
 * Timestamp pools take their native entries in units of KK_TIMESTAMP_UNIT_QUERIES
 * from the device's timestamp arena, so live pools are bounded by heap entries,
 * not by the process's live counter heaps (32 on AGX). Creation and destruction
 * take the arena's lock; writes and resolves read the pool's own chunk table.
 */
#ifndef KK_TIMESTAMP_HEAPS_H
#define KK_TIMESTAMP_HEAPS_H

#include "kosmickrisp/bridge/mtl_timestamp_arena.h"

static VkResult
kk_timestamp_alloc_heaps(struct kk_device *dev, struct kk_query_pool *pool,
                         const VkAllocationCallbacks *allocator)
{
   const uint32_t count =
      DIV_ROUND_UP(pool->vk.query_count, KK_TIMESTAMP_UNIT_QUERIES);
   struct kk_ts_chunk *chunks = vk_alloc2(
      &dev->vk.alloc, allocator,
      (size_t)count * (sizeof(*chunks) + sizeof(uint32_t)), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!chunks)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   uint32_t *units = (uint32_t *)(chunks + count);
   if (!mtl_timestamp_arena_take(dev->timestamp_arena, count, units)) {
      vk_free2(&dev->vk.alloc, allocator, chunks);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   for (uint32_t i = 0; i < count; ++i) {
      chunks[i].heap = mtl_timestamp_arena_unit(dev->timestamp_arena, units[i],
                                                NULL, &chunks[i].first);
   }
   pool->ts.chunks = chunks;
   pool->ts.chunk_count = count;
   return VK_SUCCESS;
}

static void
kk_timestamp_free_heaps(struct kk_device *dev, struct kk_query_pool *pool,
                        const VkAllocationCallbacks *allocator)
{
   if (!pool->ts.chunks)
      return;

   mtl_timestamp_arena_give(dev->timestamp_arena, pool->ts.chunk_count,
                            (const uint32_t *)(pool->ts.chunks +
                                               pool->ts.chunk_count));
   vk_free2(&dev->vk.alloc, allocator, pool->ts.chunks);
   pool->ts.chunks = NULL;
}

static inline mtl_counter_heap *
kk_timestamp_query_heap(const struct kk_query_pool *pool, uint32_t query,
                        uint32_t *index)
{
   assert(query < pool->vk.query_count);
   const struct kk_ts_chunk *chunk =
      &pool->ts.chunks[query / KK_TIMESTAMP_UNIT_QUERIES];
   *index = chunk->first + query % KK_TIMESTAMP_UNIT_QUERIES;
   return chunk->heap;
}

#endif /* KK_TIMESTAMP_HEAPS_H */
