// SPDX-License-Identifier: MIT
// Private resources share the ordinary placement-heap allocation/binding path.
export const id = 'memory-types';
export function apply({edit}) {
  const change = (file, from, to) => edit('src/kosmickrisp/' + file, (_s, replace) => replace(from, to));
  change('vulkan/kk_physical_device.c',
    '   pdev->mem_types[pdev->mem_type_count++] = (VkMemoryType){',
    `   /* A subset of the shared type's flags comes first, as Vulkan requires. */
   pdev->mem_types[pdev->mem_type_count++] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .heapIndex = sysmem_heap_idx,
   };

   pdev->mem_types[pdev->mem_type_count++] = (VkMemoryType){`);
  change('bridge/mtl_heap.m', 'options:KK_MTL_RESOURCE_OPTIONS offset:offset_B', 'options:hp.resourceOptions offset:offset_B');
  change('bridge/mtl_heap.h', 'uint64_t mtl_heap_get_size(mtl_heap *heap);',
    'uint64_t mtl_heap_get_size(mtl_heap *heap);\nenum mtl_storage_mode mtl_heap_get_storage_mode(mtl_heap *heap);');
  change('bridge/mtl_heap.m', 'static MTLTextureDescriptor *', `enum mtl_storage_mode
mtl_heap_get_storage_mode(mtl_heap *heap)
{
   return (enum mtl_storage_mode)((id<MTLHeap>)heap).storageMode;
}

static MTLTextureDescriptor *`);
  change('vulkan/kk_bo.h', 'void kk_destroy_bo(struct kk_device *dev, struct kk_bo *bo);',
    `VkResult kk_alloc_bo_options(struct kk_device *dev, struct vk_object_base *log_obj,
                             uint64_t size_B, uint64_t align_B,
                             enum mtl_resource_options options,
                             struct kk_bo **bo_out);

void kk_destroy_bo(struct kk_device *dev, struct kk_bo *bo);`);
  change('vulkan/kk_bo.c', `kk_alloc_bo(struct kk_device *dev, struct vk_object_base *log_obj,
            uint64_t size_B, uint64_t align_B, struct kk_bo **bo_out)
{`, `kk_alloc_bo(struct kk_device *dev, struct vk_object_base *log_obj,
            uint64_t size_B, uint64_t align_B, struct kk_bo **bo_out)
{
   return kk_alloc_bo_options(dev, log_obj, size_B, align_B,
                              KK_MTL_RESOURCE_OPTIONS, bo_out);
}

VkResult
kk_alloc_bo_options(struct kk_device *dev, struct vk_object_base *log_obj,
                    uint64_t size_B, uint64_t align_B,
                    enum mtl_resource_options options, struct kk_bo **bo_out)
{`);
  change('vulkan/kk_bo.c', 'mtl_new_heap(dev->mtl_handle, size_B, KK_MTL_RESOURCE_OPTIONS)', 'mtl_new_heap(dev->mtl_handle, size_B, options)');
  change('vulkan/kk_bo.c', '   bo->cpu = mtl_get_contents(map);',
    `   if ((options & (3u << MTL_RESOURCE_STORAGE_MODE_SHIFT)) ==
       MTL_RESOURCE_STORAGE_MODE_SHARED)
      bo->cpu = mtl_get_contents(map);`);
  change('vulkan/kk_device_memory.c',
    '   pMemoryMetalHandleProperties->memoryTypeBits =\n      BITFIELD_MASK(pdev->mem_type_count);',
    `   pMemoryMetalHandleProperties->memoryTypeBits = 0;
   enum mtl_storage_mode mode = mtl_heap_get_storage_mode((mtl_heap *)pHandle);
   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      bool host_visible = pdev->mem_types[i].propertyFlags &
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      if ((mode == MTL_STORAGE_MODE_SHARED && host_visible) ||
          (mode == MTL_STORAGE_MODE_PRIVATE && !host_visible))
         pMemoryMetalHandleProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }`);
  change('vulkan/kk_device_memory.c',
    '   pMemoryHostPointerProperties->memoryTypeBits =\n      BITFIELD_MASK(pdev->mem_type_count);',
    `   pMemoryHostPointerProperties->memoryTypeBits = 0;
   for (uint32_t i = 0; i < pdev->mem_type_count; i++) {
      if (pdev->mem_types[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         pMemoryHostPointerProperties->memoryTypeBits |= BITFIELD_BIT(i);
   }`);
  change('vulkan/kk_device_memory.c', '   uint64_t alignment = 4096;',
    `   struct kk_physical_device *pdev = kk_device_physical(dev);
   VkMemoryPropertyFlags properties =
      pdev->mem_types[pAllocateInfo->memoryTypeIndex].propertyFlags;
   bool host_visible = properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   enum mtl_resource_options options = MTL_RESOURCE_TRACKING_MODE_UNTRACKED |
      (host_visible ? MTL_RESOURCE_STORAGE_MODE_SHARED
                    : MTL_RESOURCE_STORAGE_MODE_PRIVATE);

   uint64_t alignment = 4096;`);
  change('vulkan/kk_device_memory.c', `         mem->bo->mtl_handle = mtl_retain(metal_info->handle);`,
    `         enum mtl_storage_mode mode = mtl_heap_get_storage_mode(metal_info->handle);
         if (mode != (host_visible ? MTL_STORAGE_MODE_SHARED : MTL_STORAGE_MODE_PRIVATE)) {
            result = vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE);
            goto fail_import;
         }
         mem->bo->mtl_handle = mtl_retain(metal_info->handle);`);
  change('vulkan/kk_device_memory.c',
    '         if (!(host_info->handleType & kk_host_mem_props.compatibleHandleTypes) ||',
    '         if (!host_visible || !(host_info->handleType & kk_host_mem_props.compatibleHandleTypes) ||');
  change('vulkan/kk_device_memory.c', '      mem->bo->cpu = mtl_get_contents(mem->bo->map);',
    '      if (host_visible)\n         mem->bo->cpu = mtl_get_contents(mem->bo->map);');
  change('vulkan/kk_device_memory.c',
    '         kk_alloc_bo(dev, &dev->vk.base, aligned_size, alignment, &mem->bo);',
    '         kk_alloc_bo_options(dev, &dev->vk.base, aligned_size, alignment, options, &mem->bo);');
  change('vulkan/kk_device_memory.c', '   const VkDeviceSize offset = pMemoryMapInfo->offset;',
    `   if (!mem->bo->cpu)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   const VkDeviceSize offset = pMemoryMapInfo->offset;`);
  change('vulkan/kk_buffer.c', '   mtl_get_contents(buffer->metal.handle);',
    '   if (mem->bo->cpu)\n      mtl_get_contents(buffer->metal.handle);');
}
