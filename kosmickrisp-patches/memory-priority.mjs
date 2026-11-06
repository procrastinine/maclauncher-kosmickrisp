// SPDX-License-Identifier: MIT
// Queue residency protects execution; high-priority allocations retain idle
// residency as well. No purgeable state is used: contents and GPU VAs survive.
export const id = 'memory-priority';
export function apply({edit}) {
  const change = (file, from, to) => edit('src/kosmickrisp/' + file, (_s, replace) => replace(from, to));
  change('vulkan/kk_physical_device.c',
    '   kk_get_device_extensions(instance, pdev, &supported_extensions);',
    `   kk_get_device_extensions(instance, pdev, &supported_extensions);
   supported_extensions.EXT_memory_priority = true;
   supported_extensions.EXT_pageable_device_local_memory = true;`);
  change('vulkan/kk_physical_device.c',
    '   kk_get_device_features(pdev, &supported_extensions, &supported_features);',
    `   kk_get_device_features(pdev, &supported_extensions, &supported_features);
   supported_features.memoryPriority = true;
   supported_features.pageableDeviceLocalMemory = true;`);
  change('vulkan/kk_device.h',
    `struct kk_residency_set {
   simple_mtx_t mutex;
   mtl_residency_set *handle;
   struct list_head allocations;
};`,
    `struct kk_residency_set {
   simple_mtx_t mutex;
   mtl_residency_set *handle;
   struct list_head allocations;
   /* Requested only for the high-priority working set, including while idle. */
   mtl_residency_set *priority_handle;
   struct list_head priority_memories;
};`);
  change('vulkan/kk_device.c',
    `   if (dev->residency_set.handle == NULL)
      goto fail_compiler;`,
    `   if (dev->residency_set.handle == NULL)
      goto fail_compiler;

   /* The optional set can fail into cleanup that destroys this mutex. */
   simple_mtx_init(&dev->blit_queue_mutex, mtx_plain);
   list_inithead(&dev->residency_set.priority_memories);
   if (dev->vk.enabled_features.memoryPriority ||
       dev->vk.enabled_features.pageableDeviceLocalMemory) {
      dev->residency_set.priority_handle = mtl_new_residency_set(dev->mtl_handle);
      if (!dev->residency_set.priority_handle) {
         result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail_vab_memory;
      }
      mtl_residency_set_request_residency(dev->residency_set.priority_handle);
   }`);
  change('vulkan/kk_device.c',
    `   simple_mtx_init(&dev->blit_queue_mutex, mtx_plain);
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {`,
    `   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {`);
  edit('src/kosmickrisp/vulkan/kk_device.c', s => s.replaceAll(
    '   mtl_release(dev->residency_set.handle);',
    `   if (dev->residency_set.priority_handle) {
      mtl_residency_set_end_residency(dev->residency_set.priority_handle);
      mtl_release(dev->residency_set.priority_handle);
   }
   mtl_release(dev->residency_set.handle);`));
  change('vulkan/kk_device.c',
    '   mtl_residency_set_request_residency(dev->residency_set.handle);',
    `   /* Every Metal queue already uses this set for execution-scoped residency.
    * Pageable mode leaves idle pages to the OS instead of requesting lifetime
    * residency for every allocation. The priority set requests extra retention. */
   if (!dev->vk.enabled_features.pageableDeviceLocalMemory)
      mtl_residency_set_request_residency(dev->residency_set.handle);`);
  change('vulkan/kk_device_memory.h',
    `   struct kk_bo *bo;
   void *map;`,
    `   struct kk_bo *bo;
   void *map;
   float priority;
   bool priority_resident;
   struct list_head priority_link;`);
  change('vulkan/kk_device_memory.c',
    'VKAPI_ATTR VkResult VKAPI_CALL\nkk_AllocateMemory(',
    `/* Metal has no per-allocation scalar priority. Use two residency classes:
 * priorities above the default 0.5 prefetch and retain idle residency, while
 * default/lower priorities use execution-scoped residency on every queue.
 * This is a residency hint, not a promise of physical page placement. */
static void
kk_device_memory_set_priority(struct kk_device *dev,
                              struct kk_device_memory *mem, float priority)
{
   mem->priority = priority;
   if (!dev->residency_set.priority_handle)
      return;

   bool retain = priority > 0.5f;
   simple_mtx_lock(&dev->residency_set.mutex);
   if (retain != mem->priority_resident) {
      mtl_allocation *allocation = mem->bo->mtl_handle
         ? (mtl_allocation *)mem->bo->mtl_handle : (mtl_allocation *)mem->bo->map;
      /* Imported VkDeviceMemory objects can refer to the same native heap.
       * Keep its one native retention request until the final high-priority
       * alias is released. Hints run at allocation/update, never per draw. */
      if (!retain)
         list_del(&mem->priority_link);
      bool retained_alias = false;
      list_for_each_entry(struct kk_device_memory, alias,
                          &dev->residency_set.priority_memories, priority_link) {
         mtl_allocation *other = alias->bo->mtl_handle
            ? (mtl_allocation *)alias->bo->mtl_handle
            : (mtl_allocation *)alias->bo->map;
         if (other == allocation) {
            retained_alias = true;
            break;
         }
      }
      if (!retained_alias) {
         if (retain)
            mtl_residency_set_add_allocation(dev->residency_set.priority_handle,
                                             allocation);
         else
            mtl_residency_set_remove_allocation(dev->residency_set.priority_handle,
                                                allocation);
         mtl_residency_set_commit(dev->residency_set.priority_handle);
      }
      if (retain)
         list_addtail(&mem->priority_link, &dev->residency_set.priority_memories);
      mem->priority_resident = retain;
   }
   simple_mtx_unlock(&dev->residency_set.mutex);
}

VKAPI_ATTR void VKAPI_CALL
kk_SetDeviceMemoryPriorityEXT(VkDevice device, VkDeviceMemory memory,
                              float priority)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_device_memory, mem, memory);
   kk_device_memory_set_priority(dev, mem, priority);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_AllocateMemory(`);
  change('vulkan/kk_device_memory.c',
    '   *pMem = kk_device_memory_to_handle(mem);',
    `   const VkMemoryPriorityAllocateInfoEXT *priority_info =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_PRIORITY_ALLOCATE_INFO_EXT);
   kk_device_memory_set_priority(dev, mem,
                                 priority_info ? priority_info->priority : 0.5f);

   *pMem = kk_device_memory_to_handle(mem);`);
  change('vulkan/kk_device_memory.c',
    `   kk_destroy_bo(dev, mem->bo);

   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);`,
    `   kk_device_memory_set_priority(dev, mem, 0.0f);
   kk_destroy_bo(dev, mem->bo);

   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);`);
}
