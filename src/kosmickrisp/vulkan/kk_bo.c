/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_bo.h"

#include "kk_device.h"
#include "kk_physical_device.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_sparse.h"

#include "util/u_memory.h"

#if DETECT_OS_APPLE
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#endif /* DETECT_OS_APPLE */

VkResult
kk_alloc_bo(struct kk_device *dev, struct vk_object_base *log_obj,
            uint64_t size_B, uint64_t align_B, struct kk_bo **bo_out)
{
   return kk_alloc_bo_options(dev, log_obj, size_B, align_B,
                              KK_MTL_RESOURCE_OPTIONS, bo_out);
}

VkResult
kk_alloc_bo_options(struct kk_device *dev, struct vk_object_base *log_obj,
                    uint64_t size_B, uint64_t align_B,
                    enum mtl_resource_options options, struct kk_bo **bo_out)
{
   VkResult result = VK_SUCCESS;

   const bool align_gpu_base = size_B >= 65536;
   const bool private_heap =
      (options & (3u << MTL_RESOURCE_STORAGE_MODE_SHIFT)) ==
      MTL_RESOURCE_STORAGE_MODE_PRIVATE;

   // TODO_KOSMICKRISP: Probably requires handling the buffer maximum 256MB
   uint64_t minimum_alignment = 0u;
   mtl_heap_buffer_size_and_align_with_length(dev->mtl_handle, &size_B,
                                              &minimum_alignment);
   minimum_alignment = MAX2(minimum_alignment, align_B);

#if DETECT_ARCH_X86_64
   /* KK_WORKAROUND_8 */
   struct kk_physical_device *pdev = kk_device_physical(dev);
   if (!(pdev->settings.disabled_workarounds & BITFIELD64_BIT(8))) {
      minimum_alignment = MAX2(minimum_alignment, 16384);
   }
#endif

   size_B = align64(size_B, minimum_alignment);
   uint64_t heap_offset_B = 0;
   uint64_t heap_size_B = size_B;
   if (align_gpu_base && private_heap) {
      if (size_B > UINT64_MAX - 65535) {
         result = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto fail_heap;
      }
      /* Preserve the existing 64 KiB placement-sparse heap constructor. Its
       * physical capacity is not part of the logical BO size. */
      heap_size_B = align64(size_B, 65536);
   }
   mtl_heap *handle = mtl_new_heap(dev->mtl_handle, heap_size_B, options);
   if (handle == NULL) {
      result = vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");
      goto fail_heap;
   }

   mtl_buffer *map = mtl_new_buffer_with_length(handle, size_B, 0u);
   if (map == NULL) {
      result = vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");
      goto fail_map;
   }

   if (align_gpu_base) {
      heap_offset_B = -mtl_buffer_get_gpu_address(map) & UINT64_C(65535);
      if (heap_offset_B && private_heap &&
          mtl_device_supports_placement_sparse(dev->mtl_handle)) {
         /* A half-page prefix cannot be expressed by sparse heap-page binds.
          * Never publish it or withdraw the private memory type. */
         result = vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                           "Native sparse-compatible heap base is not 64 KiB aligned");
         goto fail_alloc;
      }
      if (heap_offset_B > mtl_heap_get_size(handle) - size_B) {
         if (size_B > UINT64_MAX - 65536) {
            result = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            goto fail_alloc;
         }
         /* The first, unpublished heap needs more capacity. One bounded
          * replacement reserves enough for any 64 KiB alignment prefix. */
         mtl_release(map);
         mtl_release(handle);
         heap_size_B = size_B + 65536;
         handle = mtl_new_heap(dev->mtl_handle, heap_size_B, options);
         if (!handle) {
            result = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            goto fail_heap;
         }
         map = mtl_new_buffer_with_length(handle, size_B, 0);
         if (!map) {
            result = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            goto fail_map;
         }
         heap_offset_B = -mtl_buffer_get_gpu_address(map) & UINT64_C(65535);
      }
      if (heap_offset_B) {
         mtl_buffer *logical_map =
            mtl_new_buffer_with_length(handle, size_B, heap_offset_B);
         if (!logical_map) {
            result = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            goto fail_alloc;
         }
         mtl_release(map);
         map = logical_map;
      }
      if (mtl_buffer_get_gpu_address(map) & UINT64_C(65535)) {
         result = vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                           "Native logical allocation base is not 64 KiB aligned");
         goto fail_alloc;
      }
      if (heap_offset_B || mtl_heap_get_size(handle) != size_B) {
         const struct mtl_heap_memory_region region = {
            .offset_B = heap_offset_B, .size_B = size_B,
         };
         if (!mtl_heap_set_memory_region(handle, &region)) {
            result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
            goto fail_alloc;
         }
      }
   }

   struct kk_bo *bo = CALLOC_STRUCT(kk_bo);

   if (bo == NULL) {
      result = vk_errorf(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY, "%m");
      goto fail_alloc;
   }

   bo->mtl_handle = handle;
   bo->size_B = size_B;
   bo->heap_offset_B = heap_offset_B;
   bo->map = map;
   bo->gpu = mtl_buffer_get_gpu_address(map);
   if ((options & (3u << MTL_RESOURCE_STORAGE_MODE_SHIFT)) ==
       MTL_RESOURCE_STORAGE_MODE_SHARED)
      bo->cpu = mtl_get_contents(map);

   kk_device_add_bo_to_residency_set(dev, bo);

   *bo_out = bo;
   return result;

fail_alloc:
   mtl_release(map);
fail_map:
   mtl_release(handle);
fail_heap:
   return result;
}

void
kk_destroy_bo(struct kk_device *dev, struct kk_bo *bo)
{
   /* We may only have a mapped buffer, for example if the memory was imported
    * from a host pointer */
   kk_device_remove_bo_from_residency_set(dev, bo);

   mtl_release(bo->map);

   if (bo->mtl_handle)
      mtl_release(bo->mtl_handle);

   FREE(bo);
}

VkResult
kk_bo_map_placed(struct kk_device *dev, struct kk_bo *bo, void **addr)
{
#if DETECT_OS_APPLE
   mach_vm_address_t src_addr = (mach_vm_address_t)bo->cpu;
   mach_vm_address_t dst_addr = (mach_vm_address_t)*addr;

   int flags = VM_FLAGS_ANYWHERE;
   if (dst_addr)
      flags = VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE;

   vm_prot_t cur_prot;
   vm_prot_t max_prot;

   /* Metal provides us with its own CPU mapping of the memory. mach_vm_remap
    * allows us to create a second mapping pointing to that same memory, placed
    * wherever the caller requests. Note that this is used even without a fixed
    * address, since we need a unique mapping to support reserved unmap. */
   kern_return_t ret = mach_vm_remap(mach_task_self(), &dst_addr, bo->size_B, 0,
                                     flags, mach_task_self(), src_addr, false,
                                     &cur_prot, &max_prot, VM_INHERIT_DEFAULT);
   if (ret != KERN_SUCCESS)
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                       "mach_vm_remap failed: %d", ret);

   *addr = (void *)dst_addr;
#endif /* DETECT_OS_APPLE */

   return VK_SUCCESS;
}

VkResult
kk_bo_unmap(struct kk_device *dev, struct kk_bo *bo, void *addr, bool reserved)
{
#if DETECT_OS_APPLE
   mach_vm_address_t unmap_addr = (mach_vm_address_t)addr;
   if (reserved) {
      /* Replace with a reserved mapping using mach_vm_map */
      kern_return_t ret = mach_vm_map(
         mach_task_self(), &unmap_addr, bo->size_B, 0,
         VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MEMORY_OBJECT_NULL, 0, false,
         VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
      if (ret != KERN_SUCCESS)
         return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED,
                          "mach_vm_map failed: %d", ret);
   } else {
      /* According to spec, we can only return a fail code here if the reserve
       * flag is set, so ignore the result */
      mach_vm_deallocate(mach_task_self(), unmap_addr, bo->size_B);
   }
#endif /* DETECT_OS_APPLE */

   return VK_SUCCESS;
}

void
kk_bo_set_label(struct kk_bo *bo, const char *label)
{
   if (bo->mtl_handle)
      mtl_heap_set_label(bo->mtl_handle, label);
   if (bo->map)
      mtl_resource_set_label(bo->map, label);
}
