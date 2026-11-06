// SPDX-License-Identifier: MIT
export function apply({edit}) {
// A rejected Metal import must fail before size, address or residency queries.
edit('src/kosmickrisp/vulkan/kk_device_memory.c', (s, replace) => {
 const start = s.indexOf('      if (import_metal) {');
 const end = s.indexOf('      mem->bo->gpu =', start);
 if (start < 0 || end < start) throw new Error('Missing Metal import boundary');
 return s.slice(0, start) + `      if (import_metal) {
         if (metal_info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT ||
             !metal_info->handle) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         mem->bo->mtl_handle = mtl_retain(metal_info->handle);
         mem->bo->map = mtl_new_buffer_with_length(
            mem->bo->mtl_handle, mem->vk.size, 0u);
      } else {
         if (!(host_info->handleType & kk_host_mem_props.compatibleHandleTypes) ||
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
         kk_device_add_heap_to_residency_set(dev, mem->bo->mtl_handle);
      } else {
         mem->bo->size_B = mtl_buffer_get_length(mem->bo->map);
         kk_device_add_buffer_to_residency_set(dev, mem->bo->map);
      }

` + s.slice(end);
});
}
