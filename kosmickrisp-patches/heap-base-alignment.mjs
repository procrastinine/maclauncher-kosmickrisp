// SPDX-License-Identifier: MIT
// Native heap capacity and physical placement are separate from logical memory.
export const id = 'heap-base-alignment';
export function apply({edit}) {
  const change = (file, from, to) => edit('src/kosmickrisp/' + file, (_s, replace) => replace(from, to));
  change('vulkan/kk_bo.h', '   uint64_t size_B;', '   uint64_t size_B;\n   uint64_t heap_offset_B;');
  change('vulkan/kk_device_memory.h', 'extern const VkExternalMemoryProperties kk_mtlheap_mem_props;', `/* Logical VkDeviceMemory bytes to native heap placement bytes. */
static inline uint64_t
kk_device_memory_heap_offset_B(const struct kk_device_memory *mem,
                                uint64_t logical_offset_B)
{
   return mem->bo->heap_offset_B + logical_offset_B;
}

extern const VkExternalMemoryProperties kk_mtlheap_mem_props;`);
  change('bridge/mtl_heap.h', '#include <inttypes.h>', '#include <inttypes.h>\n#include <stdbool.h>');
  change('bridge/mtl_heap.h', '/* Allocation from heap */', `/* Immutable logical region follows a retained native heap across imports. */
struct mtl_heap_memory_region {
   uint64_t offset_B;
   uint64_t size_B;
};
bool mtl_heap_set_memory_region(mtl_heap *heap,
                                 const struct mtl_heap_memory_region *region);
bool mtl_heap_get_memory_region(mtl_heap *heap,
                                 struct mtl_heap_memory_region *region);

/* Allocation from heap */`);
  change('bridge/mtl_heap.m', '#include <Metal/MTLHeap.h>', '#include <Metal/MTLHeap.h>\n#import <Foundation/Foundation.h>\n#include <objc/runtime.h>');
  change('bridge/mtl_heap.m', '/* Allocation from heap */', `/* A process-wide selector key also works when another instance of the driver
 * imports a retained heap after the original VkDeviceMemory has been freed.
 * The immutable bytes own no resource and introduce no allocation owner cycle. */
static const void *
mtl_heap_memory_region_key(void)
{
   return (const void *)sel_registerName("mesa.kosmickrisp.heap-memory-region.v1");
}

bool
mtl_heap_set_memory_region(mtl_heap *heap,
                            const struct mtl_heap_memory_region *region)
{
   @autoreleasepool {
      NSData *data = [[NSData alloc] initWithBytes:region length:sizeof(*region)];
      if (!data)
         return false;
      objc_setAssociatedObject((id)heap, mtl_heap_memory_region_key(), data,
                                OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      [data release];
      return true;
   }
}

bool
mtl_heap_get_memory_region(mtl_heap *heap,
                            struct mtl_heap_memory_region *region)
{
   @autoreleasepool {
      id<MTLHeap> hp = (id<MTLHeap>)heap;
      NSData *data = objc_getAssociatedObject(hp, mtl_heap_memory_region_key());
      *region = (struct mtl_heap_memory_region){.size_B = hp.size};
      if (data) {
         if (data.length != sizeof(*region))
            return false;
         [data getBytes:region length:sizeof(*region)];
      }
      return region->offset_B <= hp.size &&
             region->size_B <= hp.size - region->offset_B;
   }
}

/* Allocation from heap */`);
  change('vulkan/kk_bo.c', '#include "kosmickrisp/bridge/mtl_bridge.h"', '#include "kosmickrisp/bridge/mtl_bridge.h"\n#include "kosmickrisp/bridge/mtl_sparse.h"');
  change('vulkan/kk_bo.c', '   // TODO_KOSMICKRISP: Probably requires handling the buffer maximum 256MB', `   const bool align_gpu_base = size_B >= 65536;
   const bool private_heap =
      (options & (3u << MTL_RESOURCE_STORAGE_MODE_SHIFT)) ==
      MTL_RESOURCE_STORAGE_MODE_PRIVATE;

   // TODO_KOSMICKRISP: Probably requires handling the buffer maximum 256MB`);
  change('vulkan/kk_bo.c', `   mtl_heap *handle =
      mtl_new_heap(dev->mtl_handle, size_B, options);`, `   uint64_t heap_offset_B = 0;
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
   mtl_heap *handle = mtl_new_heap(dev->mtl_handle, heap_size_B, options);`);
  change('vulkan/kk_bo.c', '   struct kk_bo *bo = CALLOC_STRUCT(kk_bo);', `   if (align_gpu_base) {
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

   struct kk_bo *bo = CALLOC_STRUCT(kk_bo);`);
  change('vulkan/kk_bo.c', '   bo->size_B = size_B;', '   bo->size_B = size_B;\n   bo->heap_offset_B = heap_offset_B;');
  change('vulkan/kk_device_memory.c', `   const uint64_t aligned_size =
      align64(pAllocateInfo->allocationSize, alignment);

`, '');
  change('vulkan/kk_device_memory.c', 'kk_alloc_bo_options(dev, &dev->vk.base, aligned_size, alignment, options, &mem->bo);', 'kk_alloc_bo_options(dev, &dev->vk.base, pAllocateInfo->allocationSize, alignment, options, &mem->bo);');
  change('vulkan/kk_device_memory.c', `         mem->bo->mtl_handle = mtl_retain(metal_info->handle);
         mem->bo->map = mtl_new_buffer_with_length(
            mem->bo->mtl_handle, mem->vk.size, 0u);`, `         struct mtl_heap_memory_region region;
         if (!mtl_heap_get_memory_region(metal_info->handle, &region) ||
             mem->vk.size > region.size_B) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         mem->bo->heap_offset_B = region.offset_B;
         mem->bo->mtl_handle = mtl_retain(metal_info->handle);
         mem->bo->map = mtl_new_buffer_with_length(
            mem->bo->mtl_handle, mem->vk.size, mem->bo->heap_offset_B);`);
  change('vulkan/kk_device_memory.c', '         mem->bo->size_B = mtl_heap_get_size(mem->bo->mtl_handle);', '         mem->bo->size_B = mem->vk.size;');
  change('vulkan/kk_buffer.c', '         mem->bo->mtl_handle, buffer->vk.size, info->memoryOffset);', `         mem->bo->mtl_handle, buffer->vk.size,
         kk_device_memory_heap_offset_B(mem, info->memoryOffset));`);
  change('vulkan/kk_image.c', `   return mtl_new_texture_with_descriptor(plane->mem->bo->mtl_handle, layout,
                                          mem_offset_B);`, `   return mtl_new_texture_with_descriptor(
      plane->mem->bo->mtl_handle, layout,
      kk_device_memory_heap_offset_B(plane->mem, mem_offset_B));`);
  change('vulkan/kk_image.c', '         mem->bo->mtl_handle, &array_layout, *offset_B);', `         mem->bo->mtl_handle, &array_layout,
         kk_device_memory_heap_offset_B(mem, *offset_B));`);
}
