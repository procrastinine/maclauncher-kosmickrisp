// SPDX-License-Identifier: MIT
// FD-backed BOs follow the same registration and priority lifetime as other memory.
export const id = 'external-memory-fd-residency';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_device_memory.c', source => source
    .replace('   kk_device_add_buffer_to_residency_set(dev, bo->map);',
             '   kk_device_add_bo_to_residency_set(dev, bo);')
    .replace(
      `      result = kk_allocate_fd_memory(dev, pAllocateInfo, fd_info, mem);
      if (result != VK_SUCCESS)
         goto fail_alloc;
      *pMem = kk_device_memory_to_handle(mem);
      return VK_SUCCESS;`,
      `      result = kk_allocate_fd_memory(dev, pAllocateInfo, fd_info, mem);
      if (result != VK_SUCCESS)
         goto fail_alloc;
      goto set_priority;`)
    .replace(
      `   const VkMemoryPriorityAllocateInfoEXT *priority_info =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_PRIORITY_ALLOCATE_INFO_EXT);
   kk_device_memory_set_priority(dev, mem,
                                 priority_info ? priority_info->priority : 0.5f);`,
      `set_priority:
   {
      const VkMemoryPriorityAllocateInfoEXT *priority_info =
         vk_find_struct_const(pAllocateInfo->pNext, MEMORY_PRIORITY_ALLOCATE_INFO_EXT);
      kk_device_memory_set_priority(dev, mem,
                                    priority_info ? priority_info->priority : 0.5f);
   }`));
}
