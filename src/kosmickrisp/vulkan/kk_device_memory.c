/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_device_memory.h"
#include "kk_sparse.h"
#include "kk_buffer.h"
#include "kk_image.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

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

static VkResult kk_device_memory_zero_init(struct kk_device *, struct kk_device_memory *);


/* OPAQUE_FD is an owning capability for this allocation, not a host pointer.
 * All supported Darwin CPUs are little-endian. The v1 header consists only
 * of fixed-width fields, with no native pointers or compiler-dependent enums.
 */
#define KK_FD_PAYLOAD_OFFSET UINT64_C(65536)
struct kk_fd_memory_header {
   uint64_t magic, version, allocation_size, payload_size, memory_type;
   uint8_t device_uuid[VK_UUID_SIZE], driver_uuid[VK_UUID_SIZE];
   uint64_t dedicated[12];
};
_Static_assert(sizeof(struct kk_fd_memory_header) < KK_FD_PAYLOAD_OFFSET,
               "External memory header must fit before the payload");

/* Only FD-backed allocations carry this local export metadata. The native
 * buffer owns the descriptor through final release; the live BO borrows it. */
struct kk_fd_bo {
   struct kk_bo base;
   int fd;
};

const VkExternalMemoryProperties kk_fd_mem_props = {
   .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
   .exportFromImportedHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
};

static void
kk_fd_dedicated_description(const VkMemoryAllocateInfo *info, uint64_t out[12])
{
   const VkMemoryDedicatedAllocateInfo *dedicated =
      vk_find_struct_const(info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (!dedicated)
      return;
   if (dedicated->image) {
      VK_FROM_HANDLE(kk_image, image, dedicated->image);
      const struct vk_image *v = &image->vk;
      out[0] = 1;
      out[1] = v->format;
      out[2] = v->extent.width;
      out[3] = v->extent.height;
      out[4] = v->extent.depth;
      out[5] = v->mip_levels;
      out[6] = v->array_layers;
      out[7] = v->samples;
      out[8] = v->tiling;
      out[9] = v->usage;
      out[10] = v->create_flags;
      out[11] = v->stencil_usage;
   } else if (dedicated->buffer) {
      VK_FROM_HANDLE(kk_buffer, buffer, dedicated->buffer);
      out[0] = 2;
      out[1] = buffer->vk.size;
      out[2] = buffer->vk.usage;
      out[3] = buffer->vk.create_flags;
   }
}

static VkResult
kk_allocate_fd_memory(struct kk_device *dev, const VkMemoryAllocateInfo *info,
                      const VkImportMemoryFdInfoKHR *import,
                      uint64_t page_size, struct kk_device_memory *mem)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
   if (!info->allocationSize || info->allocationSize > pdev->info.max_buffer_size ||
       KK_FD_PAYLOAD_OFFSET % page_size)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   const uint64_t payload_size = align64(info->allocationSize, page_size);
   if (payload_size > SIZE_MAX - KK_FD_PAYLOAD_OFFSET)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   const size_t mapping_size = KK_FD_PAYLOAD_OFFSET + payload_size;
   const bool importing = import && import->handleType;
   struct kk_fd_memory_header expected = {
      .magic = UINT64_C(0x3144464d454d4b4b),
      .version = 1,
      .allocation_size = info->allocationSize,
      .payload_size = payload_size,
      .memory_type = info->memoryTypeIndex,
   };
   memcpy(expected.device_uuid, pdev->vk.properties.deviceUUID, VK_UUID_SIZE);
   memcpy(expected.driver_uuid, pdev->vk.properties.driverUUID, VK_UUID_SIZE);
   kk_fd_dedicated_description(info, expected.dedicated);

   int fd = -1;
   if (importing) {
      if (import->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
          import->fd < 0)
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      struct stat stat;
      if (fstat(import->fd, &stat) || stat.st_size < 0 ||
          (uint64_t)stat.st_size != mapping_size)
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      /* Until the complete import succeeds the caller retains its descriptor. */
      fd = fcntl(import->fd, F_DUPFD_CLOEXEC, 0);
   } else {
      char name[32];
      for (unsigned attempt = 0; attempt < 16; ++attempt) {
         snprintf(name, sizeof(name), "/kk-%08x%08x", arc4random(), arc4random());
         fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
         if (fd >= 0 || errno != EEXIST)
            break;
      }
      if (fd >= 0) {
         if (shm_unlink(name) || fcntl(fd, F_SETFD, FD_CLOEXEC) ||
             ftruncate(fd, mapping_size)) {
            close(fd);
            return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         }
      }
   }
   if (fd < 0)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
   if (mapping == MAP_FAILED) {
      close(fd);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   if (importing) {
      if (memcmp(mapping, &expected, sizeof(expected))) {
         munmap(mapping, mapping_size);
         close(fd);
         return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      }
   } else {
      memcpy(mapping, &expected, sizeof(expected));
      /* A fresh shm object is zero-filled, including the entire payload.
       * Imported contents are never overwritten or reinitialized. */
   }

   struct kk_fd_bo *fd_bo = CALLOC_STRUCT(kk_fd_bo);
   if (!fd_bo) {
      munmap(mapping, mapping_size);
      close(fd);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   struct kk_bo *bo = &fd_bo->base;
   bo->map = mtl_new_buffer_with_memory_fd(dev->mtl_handle, fd, mapping,
                                           mapping_size, KK_FD_PAYLOAD_OFFSET,
                                           payload_size);
   if (!bo->map) {
      FREE(bo);
      munmap(mapping, mapping_size);
      close(fd);
      return vk_error(dev, importing ? VK_ERROR_INVALID_EXTERNAL_HANDLE :
                                      VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   /* The native buffer now owns its FD and mapping through final release.
    * Exported descriptors own additional kernel references independently. */
   fd_bo->fd = fd;
   bo->size_B = payload_size;
   bo->gpu = mtl_buffer_get_gpu_address(bo->map);
   if (pdev->mem_types[info->memoryTypeIndex].propertyFlags &
       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      bo->cpu = mtl_get_contents(bo->map);
   kk_device_add_bo_to_residency_set(dev, bo);
   mem->bo = bo;
   if (importing)
      close(import->fd);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *info, int *fd)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_device_memory, mem, info->memory);
   if (info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
       !(mem->vk.export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
      return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   struct kk_fd_bo *bo = container_of(mem->bo, struct kk_fd_bo, base);
   *fd = fcntl(bo->fd, F_DUPFD_CLOEXEC, 0);
   if (*fd < 0)
      return vk_error(dev, errno == EMFILE || errno == ENFILE ?
                            VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetMemoryFdPropertiesKHR(VkDevice device,
                            VkExternalMemoryHandleTypeFlagBits handleType,
                            int fd, VkMemoryFdPropertiesKHR *properties)
{
   /* OPAQUE_FD is expressly excluded from this query by VUID-00674.
    * No other memory-FD type is advertised by this driver. */
   return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

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
      if (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
         continue;
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
   const VkImportMemoryMetalHandleInfoEXT *metal_info = NULL;
   const VkImportMemoryHostPointerInfoEXT *host_info = NULL;
   const VkImportMemoryFdInfoKHR *fd_info = NULL;
   const VkMemoryPriorityAllocateInfoEXT *priority_info = NULL;

   /* Parse allocation parameters once for all backing types. In particular,
    * ordinary allocations need no separate external-FD extension-chain walk. */
   vk_foreach_struct_const(sType, info, pAllocateInfo->pNext) {
      switch (sType) {
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT:
         metal_info = info;
         break;
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
         host_info = info;
         break;
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
         fd_info = info;
         break;
      case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT:
         priority_info = info;
         break;
      default:
         break;
      }
   }

   struct kk_physical_device *pdev = kk_device_physical(dev);
   VkMemoryPropertyFlags properties =
      pdev->mem_types[pAllocateInfo->memoryTypeIndex].propertyFlags;
   bool host_visible = properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   enum mtl_resource_options options = MTL_RESOURCE_TRACKING_MODE_UNTRACKED |
      (host_visible ? MTL_RESOURCE_STORAGE_MODE_SHARED
                    : MTL_RESOURCE_STORAGE_MODE_PRIVATE);

   uint64_t alignment = 4096;
   os_get_page_size(&alignment);

   mem = vk_device_memory_create(&dev->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (!mem)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Lazy attachments and standard sparse-3D page aliases share this lock.
    * The initializer needs no native mutex until its first lock operation. */
   mem->transient_mutex = (simple_mtx_t)SIMPLE_MTX_INITIALIZER;

   if ((fd_info && fd_info->handleType) ||
       (mem->vk.export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
      if ((metal_info && metal_info->handleType) ||
          (host_info && host_info->handleType) ||
          (mem->vk.export_handle_types & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
         result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto fail_alloc;
      }
      result = kk_allocate_fd_memory(dev, pAllocateInfo, fd_info, alignment, mem);
      if (result != VK_SUCCESS)
         goto fail_alloc;
      goto set_priority;
   }

   mem->lazy = properties & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

   bool import_metal = metal_info && metal_info->handleType;
   bool import_host = host_info && host_info->handleType;

   if (mem->lazy && !import_metal && !import_host) {
      /* Tile storage has no persistent allocation until it is needed. */
   } else if (import_metal || import_host) {
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
         struct mtl_heap_memory_region region;
         if (!mtl_heap_get_memory_region(metal_info->handle, &region) ||
             mem->vk.size > region.size_B) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         mem->bo->heap_offset_B = region.offset_B;
         mem->bo->mtl_handle = mtl_retain(metal_info->handle);
         mem->bo->map = mtl_new_buffer_with_length(
            mem->bo->mtl_handle, mem->vk.size, mem->bo->heap_offset_B);
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
         mem->bo->size_B = mem->vk.size;

      } else {
         mem->bo->size_B = mtl_buffer_get_length(mem->bo->map);

      }

      kk_device_add_bo_to_residency_set(dev, mem->bo);
      mem->bo->gpu = mtl_buffer_get_gpu_address(mem->bo->map);
      if (host_visible)
         mem->bo->cpu = mtl_get_contents(mem->bo->map);
   } else {
      result =
         kk_alloc_bo_options(dev, &dev->vk.base, pAllocateInfo->allocationSize, alignment, options, &mem->bo);
      if (result != VK_SUCCESS)
         goto fail_alloc;

      /* Imports never enter this branch. VUID-flags-10760 forbids the zero
       * allocation flag on an import; supplied contents must be preserved.
       */
      result = kk_device_memory_zero_init(dev, mem);
      if (result != VK_SUCCESS) {
         kk_destroy_bo(dev, mem->bo);
         mem->bo = NULL;
         goto fail_alloc;
      }
   }

set_priority:
   kk_device_memory_set_priority(dev, mem,
                                 priority_info ? priority_info->priority : 0.5f);

   /* Only successful live allocations can require transient scans. */
   if (mem->lazy)
      p_atomic_inc(&dev->live_lazy_allocations);
   *pMem = kk_device_memory_to_handle(mem);

   return VK_SUCCESS;

fail_alloc:
   simple_mtx_destroy(&mem->transient_mutex);
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

   if (mem->standard_3d)
      kk_standard_3d_memory_finish(dev, mem);
   kk_device_memory_set_priority(dev, mem, 0.0f);
   if (mem->bo)
      kk_destroy_bo(dev, mem->bo);
   if (mem->lazy)
      p_atomic_dec(&dev->live_lazy_allocations);
   simple_mtx_destroy(&mem->transient_mutex);

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

   if (!mem->bo || !mem->bo->cpu)
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

   if (mem->lazy) {
      simple_mtx_lock(&mem->transient_mutex);
      *pCommittedMemoryInBytes = mem->bo ? mem->vk.size : 0;
      simple_mtx_unlock(&mem->transient_mutex);
   } else {
      *pCommittedMemoryInBytes = mem->vk.size;
   }
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
   if (!mem->bo || !mem->bo->mtl_handle)
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

   return mem->bo ? mem->bo->gpu : 0;
}

void
kk_device_memory_set_label(struct kk_device_memory *dev_mem, const char *label)
{
   if (dev_mem->bo)
      kk_bo_set_label(dev_mem->bo, label);
}

/* Shared initialization for immediate and deferred private commitment. */
static VkResult
kk_device_memory_zero_init(struct kk_device *dev, struct kk_device_memory *mem)
{
   if (!(mem->vk.alloc_flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT))
      return VK_SUCCESS;
   if (mem->bo->cpu) {
      memset(mem->bo->cpu, 0, mem->bo->size_B);
      return VK_SUCCESS;
   }
   enum mtl_command_queue_error error =
      mtl_buffer_zero_sync(dev->mtl_handle, mem->bo->map);
   if (error == MTL_COMMAND_QUEUE_ERROR_NONE)
      return VK_SUCCESS;
   if (error == MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   return vk_device_set_lost(&dev->vk, "Device memory zero fill failed");
}

/* The caller holds transient_mutex. Use the ordinary private heap path. */
VkResult
kk_device_memory_commit_private(struct kk_device *dev, struct kk_device_memory *mem)
{
   if (mem->bo)
      return VK_SUCCESS;
   uint64_t alignment = 4096;
   os_get_page_size(&alignment);
   VkResult result = kk_alloc_bo_options(dev, &dev->vk.base,
      align64(mem->vk.size, alignment), alignment,
      MTL_RESOURCE_TRACKING_MODE_UNTRACKED | MTL_RESOURCE_STORAGE_MODE_PRIVATE,
      &mem->bo);
   if (result == VK_SUCCESS)
      result = kk_device_memory_zero_init(dev, mem);
   if (result != VK_SUCCESS && mem->bo) {
      kk_destroy_bo(dev, mem->bo);
      mem->bo = NULL;
   }
   return result;
}
