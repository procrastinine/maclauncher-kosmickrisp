/* SPDX-License-Identifier: MIT
 * Timestamp pools retain their linear Vulkan reports while native Metal heaps
 * are allocated at capacities the device has actually accepted.
 */
#ifndef KK_TIMESTAMP_HEAPS_H
#define KK_TIMESTAMP_HEAPS_H

static VkResult
kk_timestamp_alloc_heaps(struct kk_device *dev, struct kk_query_pool *pool,
                         const VkAllocationCallbacks *allocator)
{
   uint32_t cached = p_atomic_read(&dev->timestamp_heap_capacity);
   uint32_t count = cached ? MIN2(cached, pool->vk.query_count) :
                             pool->vk.query_count;
   uint32_t failed_count = 0;

   /* Preserve the ordinary one-heap request. Only a rejected allocation takes
    * the bounded negotiation path; this is not an assumed hardware maximum. */
   while (count) {
      pool->ts.heap = mtl_new_timestamp_counter_heap(dev->mtl_handle, count);
      if (pool->ts.heap)
         break;

      failed_count = count;
      count /= 2;
   }

   if (!pool->ts.heap)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   pool->ts.heap_capacity = mtl_timestamp_counter_heap_count(pool->ts.heap);
   if (!pool->ts.heap_capacity)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   if (failed_count) {
      /* Search only the nearest rejected/accepted interval. Retain the known
       * successful heap until a larger allocation succeeds, including odd
       * requests such as capacity+1. No device-limit API is assumed. */
      uint32_t upper = failed_count - 1;
      while (pool->ts.heap_capacity < upper) {
         uint32_t middle = pool->ts.heap_capacity +
            (upper - pool->ts.heap_capacity + 1) / 2;
         mtl_counter_heap *candidate =
            mtl_new_timestamp_counter_heap(dev->mtl_handle, middle);
         if (!candidate) {
            upper = middle - 1;
            continue;
         }

         uint32_t actual = mtl_timestamp_counter_heap_count(candidate);
         if (actual > pool->ts.heap_capacity) {
            mtl_release(pool->ts.heap);
            pool->ts.heap = candidate;
            pool->ts.heap_capacity = actual;
         } else {
            mtl_release(candidate);
         }
         if (actual < middle)
            upper = middle - 1;
      }

      /* Publish the largest actually accepted count across concurrent
       * negotiations. This is cached capacity, not an exposed hardware max. */
      uint32_t observed = p_atomic_read(&dev->timestamp_heap_capacity);
      while (observed < pool->ts.heap_capacity) {
         uint32_t previous = p_atomic_cmpxchg(
            &dev->timestamp_heap_capacity, observed, pool->ts.heap_capacity);
         if (previous == observed)
            break;
         observed = previous;
      }
   }

   if (pool->ts.heap_capacity >= pool->vk.query_count)
      return VK_SUCCESS;

   /* Keep the original first heap separate: a single-heap pool allocates no
    * heap pointer array and uses the original query index directly. */
   pool->ts.extra_count = (pool->vk.query_count - 1) / pool->ts.heap_capacity;
   pool->ts.extra_heaps = vk_zalloc2(
      &dev->vk.alloc, allocator,
      (size_t)pool->ts.extra_count * sizeof(*pool->ts.extra_heaps), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pool->ts.extra_heaps)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   for (uint32_t i = 0; i < pool->ts.extra_count; ++i) {
      uint32_t first = (i + 1) * pool->ts.heap_capacity;
      uint32_t length = MIN2(pool->ts.heap_capacity,
                             pool->vk.query_count - first);
      pool->ts.extra_heaps[i] =
         mtl_new_timestamp_counter_heap(dev->mtl_handle, length);
      if (!pool->ts.extra_heaps[i] ||
          mtl_timestamp_counter_heap_count(pool->ts.extra_heaps[i]) < length)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   return VK_SUCCESS;
}

static inline mtl_counter_heap *
kk_timestamp_query_heap(const struct kk_query_pool *pool, uint32_t query,
                         uint32_t *index)
{
   assert(query < pool->vk.query_count);
   if (!pool->ts.extra_heaps) {
      *index = query;
      return pool->ts.heap;
   }

   uint32_t chunk = query / pool->ts.heap_capacity;
   *index = query % pool->ts.heap_capacity;
   return chunk ? pool->ts.extra_heaps[chunk - 1] : pool->ts.heap;
}

#endif /* KK_TIMESTAMP_HEAPS_H */
