/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_device_memory.h"

#include "kk_device.h"
#include "kk_entrypoints.h"
#include "kk_physical_device.h"

#include "kosmickrisp/bridge/mtl_bridge.h"

#include "vulkan/vulkan_metal.h"

#include "util/os_misc.h"
#include "util/u_atomic.h"
#include "util/u_memory.h"

#include <inttypes.h>
#include <string.h>
#include <sys/mman.h>

/* Supports mtlheap only */
const VkExternalMemoryProperties kk_mtlheap_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT,
   .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT,
};

const VkExternalMemoryProperties kk_host_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
   .exportFromImportedHandleTypes = 0,
   .compatibleHandleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT,
};

#ifdef VK_USE_PLATFORM_METAL_EXT
VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryMetalHandlePropertiesEXT(
   VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType,
   const void *pHandle,
   VkMemoryMetalHandlePropertiesEXT *pMemoryMetalHandleProperties)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   /* We only support heaps since that's the backing for all our memory and
    * simplifies implementation */
   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT:
      break;
   default:
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
   pMemoryMetalHandleProperties->memoryTypeBits = 0;
   enum mtl_storage_mode mode = mtl_heap_get_storage_mode((mtl_heap *)pHandle);
   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      bool host_visible = pdev->mem_types[i].propertyFlags &
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      if ((mode == MTL_STORAGE_MODE_SHARED && host_visible) ||
          (mode == MTL_STORAGE_MODE_PRIVATE && !host_visible))
         pMemoryMetalHandleProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }

   return VK_SUCCESS;
}
#endif /* VK_USE_PLATFORM_METAL_EXT */

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryHostPointerPropertiesEXT(
   VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType,
   UNUSED const void *pHandle,
   VkMemoryHostPointerPropertiesEXT *pMemoryHostPointerProperties)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT:
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT:
      break;
   default:
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
   pMemoryHostPointerProperties->memoryTypeBits = 0;
   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      if (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         pMemoryHostPointerProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }

   return VK_SUCCESS;
}

/* Metal has no per-allocation scalar priority. Use two residency classes:
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
kk_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                  const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_device_memory *mem;
   VkResult result = VK_SUCCESS;
   const VkImportMemoryMetalHandleInfoEXT *metal_info = vk_find_struct_const(
      pAllocateInfo->pNext, IMPORT_MEMORY_METAL_HANDLE_INFO_EXT);
   const VkImportMemoryHostPointerInfoEXT *host_info = vk_find_struct_const(
      pAllocateInfo->pNext, IMPORT_MEMORY_HOST_POINTER_INFO_EXT);

   struct kk_physical_device *pdev = kk_device_physical(dev);
   VkMemoryPropertyFlags properties =
      pdev->mem_types[pAllocateInfo->memoryTypeIndex].propertyFlags;
   bool host_visible = properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   enum mtl_resource_options options = MTL_RESOURCE_TRACKING_MODE_UNTRACKED |
      (host_visible ? MTL_RESOURCE_STORAGE_MODE_SHARED
                    : MTL_RESOURCE_STORAGE_MODE_PRIVATE);

   uint64_t alignment = 4096;
   os_get_page_size(&alignment);

   const uint64_t aligned_size =
      align64(pAllocateInfo->allocationSize, alignment);

   mem = vk_device_memory_create(&dev->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (!mem)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   bool import_metal = metal_info && metal_info->handleType;
   bool import_host = host_info && host_info->handleType;

   if (import_metal || import_host) {
      mem->bo = CALLOC_STRUCT(kk_bo);
      if (!mem->bo) {
         result = vk_errorf(&dev->vk.base, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");
         goto fail_alloc;
      }

      if (import_metal) {
         if (metal_info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT ||
             !metal_info->handle) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         enum mtl_storage_mode mode = mtl_heap_get_storage_mode(metal_info->handle);
         if (mode != (host_visible ? MTL_STORAGE_MODE_SHARED : MTL_STORAGE_MODE_PRIVATE)) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         mem->bo->mtl_handle = mtl_retain(metal_info->handle);
         mem->bo->map = mtl_new_buffer_with_length(
            mem->bo->mtl_handle, mem->vk.size, 0u);
      } else {
         if (!host_visible || !(host_info->handleType & kk_host_mem_props.compatibleHandleTypes) ||
             !host_info->pHostPointer ||
             (uintptr_t)host_info->pHostPointer % alignment) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         mem->bo->map = mtl_new_buffer_with_bytes_no_copy(
            dev->mtl_handle, host_info->pHostPointer, mem->vk.size);
      }
      if (!mem->bo->map) {
         result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
fail_import:
         if (mem->bo->mtl_handle) mtl_release(mem->bo->mtl_handle);
         FREE(mem->bo);
         mem->bo = NULL;
         goto fail_alloc;
      }
      if (import_metal) {
         mem->bo->size_B = mtl_heap_get_size(mem->bo->mtl_handle);

      } else {
         mem->bo->size_B = mtl_buffer_get_length(mem->bo->map);

      }

      kk_device_add_bo_to_residency_set(dev, mem->bo);
      mem->bo->gpu = mtl_buffer_get_gpu_address(mem->bo->map);
      if (host_visible)
         mem->bo->cpu = mtl_get_contents(mem->bo->map);
   } else {
      result =
         kk_alloc_bo_options(dev, &dev->vk.base, aligned_size, alignment, options, &mem->bo);
      if (result != VK_SUCCESS)
         goto fail_alloc;

      /* Imports never enter this branch. VUID-flags-10760 forbids the zero
       * allocation flag on an import; supplied contents must be preserved.
       */
      if (mem->vk.alloc_flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT) {
         if (mem->bo->cpu) {
            memset(mem->bo->cpu, 0, mem->bo->size_B);
         } else {
            enum mtl_command_queue_error error =
               mtl_buffer_zero_sync(dev->mtl_handle, mem->bo->map);
            if (error != MTL_COMMAND_QUEUE_ERROR_NONE) {
               kk_destroy_bo(dev, mem->bo);
               if (error == MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY)
                  result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
               else
                  result = vk_device_set_lost(&dev->vk,
                                             "Device memory zero fill failed");
               goto fail_alloc;
            }
         }
      }
   }

   const VkMemoryPriorityAllocateInfoEXT *priority_info =
      vk_find_struct_const(pAllocateInfo->pNext, MEMORY_PRIORITY_ALLOCATE_INFO_EXT);
   kk_device_memory_set_priority(dev, mem,
                                 priority_info ? priority_info->priority : 0.5f);

   *pMem = kk_device_memory_to_handle(mem);

   return VK_SUCCESS;

fail_alloc:
   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
kk_FreeMemory(VkDevice device, VkDeviceMemory _mem,
              const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_device_memory, mem, _mem);

   if (!mem)
      return;

   /* According to spec, memory is implicitly unmapped upon free */
   if (mem->map)
      kk_bo_unmap(dev, mem->bo, mem->map, false);

   kk_device_memory_set_priority(dev, mem, 0.0f);
   kk_destroy_bo(dev, mem->bo);

   vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_MapMemory2KHR(VkDevice device, const VkMemoryMapInfoKHR *pMemoryMapInfo,
                 void **ppData)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_device_memory, mem, pMemoryMapInfo->memory);
   VkResult result = VK_SUCCESS;

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (!mem->bo->cpu)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size = vk_device_memory_range(
      &mem->vk, pMemoryMapInfo->offset, pMemoryMapInfo->size);

   void *fixed_addr = NULL;
   if (pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) {
      const VkMemoryMapPlacedInfoEXT *placed_info = vk_find_struct_const(
         pMemoryMapInfo->pNext, MEMORY_MAP_PLACED_INFO_EXT);
      fixed_addr = placed_info->pPlacedAddress;
   }

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size_B);

   if (size != (size_t)size) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%" PRIx64 " does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");
   }

   void *mapped_addr = fixed_addr;
   result = kk_bo_map_placed(dev, mem->bo, &mapped_addr);
   if (result != VK_SUCCESS)
      return result;

   assert(!fixed_addr || mapped_addr == fixed_addr);
   mem->map = mapped_addr;
   *ppData = mem->map + offset;

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_UnmapMemory2KHR(VkDevice device,
                   const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_device_memory, mem, pMemoryUnmapInfo->memory);
   VkResult result = VK_SUCCESS;

   if (mem == NULL)
      return result;

   bool reserved = pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT;

   result = kk_bo_unmap(dev, mem->bo, mem->map, reserved);
   if (result != VK_SUCCESS)
      return result;

   /* According to spec, memory is not unmapped on failure, so we only clear to
    * NULL on success */
   mem->map = NULL;

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_FlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                           const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_InvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount,
                                const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
kk_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory _mem,
                             VkDeviceSize *pCommittedMemoryInBytes)
{
   VK_FROM_HANDLE(kk_device_memory, mem, _mem);

   *pCommittedMemoryInBytes = mem->bo->size_B;
}

#ifdef VK_USE_PLATFORM_METAL_EXT
VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryMetalHandleEXT(
   VkDevice device, const VkMemoryGetMetalHandleInfoEXT *pGetMetalHandleInfo,
   void **pHandle)
{
   /* We only support heaps since that's the backing for all our memory and
    * simplifies implementation */
   assert(pGetMetalHandleInfo->handleType ==
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT);
   VK_FROM_HANDLE(kk_device_memory, mem, pGetMetalHandleInfo->memory);

   /* In some cases, such as host pointer imports, we don't have a MTLHeap */
   if (!mem->bo->mtl_handle)
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   /* From the Vulkan spec of vkGetMemoryMetalHandleEXT:
    *
    *    "Unless the app retains the handle object returned by the call,
    *     the lifespan will be the same as the associated VkDeviceMemory"
    */
   *pHandle = mem->bo->mtl_handle;
   return VK_SUCCESS;
}
#endif /* VK_USE_PLATFORM_METAL_EXT */

VKAPI_ATTR uint64_t VKAPI_CALL
kk_GetDeviceMemoryOpaqueCaptureAddress(
   UNUSED VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   VK_FROM_HANDLE(kk_device_memory, mem, pInfo->memory);

   return mem->bo->gpu;
}

void
kk_device_memory_set_label(struct kk_device_memory *dev_mem, const char *label)
{
   if (dev_mem->bo)
      kk_bo_set_label(dev_mem->bo, label);
}
