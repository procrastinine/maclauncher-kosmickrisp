// SPDX-License-Identifier: MIT
import fs from 'node:fs';

/** Back large Vulkan timestamp pools with dynamically accepted Metal chunks. */
export const id = 'timestamp-counter-heaps';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-timestamp-heaps.h'];

export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_device.h', s => s.replace(
    '   struct kk_query_table occlusion_queries;',
    `   struct kk_query_table occlusion_queries;

   /* Accepted native timestamp heap capacity, published after negotiation. */
   uint32_t timestamp_heap_capacity;`));

  edit('src/kosmickrisp/vulkan/kk_query_pool.h', s => s.replace(
    '   mtl_counter_heap *heap;',
    `   mtl_counter_heap *heap;
   mtl_counter_heap **extra_heaps;
   uint32_t heap_capacity;
   uint32_t extra_count;`));

  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    `struct kk_ts_stage_entry {
   mtl_counter_heap *heap;`,
    `struct kk_ts_stage_entry {
   const struct kk_query_pool *pool;
   mtl_counter_heap *heap;`));

  edit('src/kosmickrisp/bridge/mtl_device.h', s => s.replace(
    `mtl_counter_heap *mtl_new_timestamp_counter_heap(mtl_device *dev,
                                                 uint32_t count);`,
    `mtl_counter_heap *mtl_new_timestamp_counter_heap(mtl_device *dev,
                                                 uint32_t count);
uint32_t mtl_timestamp_counter_heap_count(mtl_counter_heap *heap);`));

  edit('src/kosmickrisp/bridge/mtl_device.m', s => s.replace(
    '/* Resource queries */',
    `uint32_t
mtl_timestamp_counter_heap_count(mtl_counter_heap *heap)
{
   @autoreleasepool {
      return (uint32_t)[(id<MTL4CounterHeap>)heap count];
   }
}

/* Resource queries */`));

  edit('src/kosmickrisp/bridge/stubs/mtl_device.c', s => s.replace(
    '/* Resource queries */',
    `uint32_t
mtl_timestamp_counter_heap_count(mtl_counter_heap *heap)
{
   return 0u;
}

/* Resource queries */`));

  edit('src/kosmickrisp/vulkan/kk_query_pool.c', s => s
    .replace('VKAPI_ATTR VkResult VKAPI_CALL\nkk_CreateQueryPool',
      '#include "kk_timestamp_heaps.h"\n\nVKAPI_ATTR VkResult VKAPI_CALL\nkk_CreateQueryPool')
    .replace(
      `      pool->ts.heap =
         mtl_new_timestamp_counter_heap(dev->mtl_handle, pool->vk.query_count);
      if (pool->ts.heap == NULL) {
         kk_DestroyQueryPool(device, kk_query_pool_to_handle(pool), pAllocator);
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }`,
      `      result = kk_timestamp_alloc_heaps(dev, pool, pAllocator);
      if (result != VK_SUCCESS) {
         kk_DestroyQueryPool(device, kk_query_pool_to_handle(pool), pAllocator);
         return vk_error(dev, result);
      }`)
    .replace(
      `      if (pool->ts.heap)
         mtl_release(pool->ts.heap);`,
      `      if (pool->ts.heap)
         mtl_release(pool->ts.heap);
      if (pool->ts.extra_heaps) {
         for (uint32_t i = 0; i < pool->ts.extra_count; ++i) {
            if (pool->ts.extra_heaps[i])
               mtl_release(pool->ts.extra_heaps[i]);
         }
         vk_free2(&dev->vk.alloc, pAllocator, pool->ts.extra_heaps);
      }`)
    .replace('   uint32_t heap_index = query;',
      `   uint32_t heap_index;
   mtl_counter_heap *heap = kk_timestamp_query_heap(pool, query, &heap_index);`)
    .replace('         if (entry->stage == mtl_stage && entry->heap == pool->ts.heap) {',
      '         if (entry->stage == mtl_stage && entry->pool == pool) {')
    .replace('            heap_index = entry->index;',
      `            heap = entry->heap;
            heap_index = entry->index;`)
    .replace(
      `         struct kk_ts_stage_entry entry = {.heap = pool->ts.heap,
                                           .stage = mtl_stage,
                                           .index = query};`,
      `         struct kk_ts_stage_entry entry = {.pool = pool,
                                           .heap = heap,
                                           .stage = mtl_stage,
                                           .index = heap_index};`)
    .replace(
      `         mtl_render_write_timestamp(cmd->metal.render, mtl_stage, pool->ts.heap,
                                    query);`,
      `         mtl_render_write_timestamp(cmd->metal.render, mtl_stage, heap,
                                    heap_index);`)
    .replace('      mtl_compute_write_timestamp(cmd->metal.compute, pool->ts.heap, query);',
      '      mtl_compute_write_timestamp(cmd->metal.compute, heap, heap_index);')
    .replace('         .heap = pool->ts.heap,', '         .heap = heap,'));

  edits.set('src/kosmickrisp/vulkan/kk_timestamp_heaps.h',
    fs.readFileSync(new URL('./kosmickrisp-timestamp-heaps.h', import.meta.url), 'utf8'));
}
