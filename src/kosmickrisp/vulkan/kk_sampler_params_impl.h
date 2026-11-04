/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_PARAMS_IMPL_H
#define KK_SAMPLER_PARAMS_IMPL_H

#include "kk_sampler_params_api.h"

/* One device-retained arena serves sampler metadata and original parameters.
 * Caller holds descriptor_heap_lock across allocation and publication. */
static VkResult
kk_heap_metadata_reserve_locked(struct kk_device *dev, uint32_t size,
                               uint32_t alignment, void **cpu_address,
                               uint64_t *gpu_address)
{
   assert(cpu_address && gpu_address);
   assert(size && size <= KK_HEAP_METADATA_PAGE_SIZE);
   assert(util_is_power_of_two_nonzero(alignment) && alignment <= 16);
   *cpu_address = NULL;
   *gpu_address = 0;
   struct kk_heap_metadata_page *page = dev->heap_metadata_pages;
   uint32_t offset = page ? ALIGN_POT(page->used, alignment) : 0;
   if (!page || offset > KK_HEAP_METADATA_PAGE_SIZE - size) {
      page = vk_zalloc(&dev->vk.alloc, sizeof(*page), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!page)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      VkResult result = kk_alloc_bo(dev, &dev->vk.base,
         KK_HEAP_METADATA_PAGE_SIZE, 16, &page->bo);
      if (result != VK_SUCCESS) {
         vk_free(&dev->vk.alloc, page);
         return result;
      }
      page->next = dev->heap_metadata_pages;
      dev->heap_metadata_pages = page;
      offset = 0;
   }
   assert(page->bo->cpu && page->bo->gpu);
   assert(page->bo->gpu <= UINT64_MAX - offset);
   assert(((page->bo->gpu + offset) & (alignment - 1)) == 0);
   *cpu_address = (uint8_t *)page->bo->cpu + offset;
   page->used = offset + size;
   *gpu_address = page->bo->gpu + offset;
   return VK_SUCCESS;
}


/* Copying records and in-place arrays share the same reserve/publication path.
 * The caller retains descriptor_heap_lock until the data is initialized. */
static VkResult
kk_heap_metadata_alloc_locked(struct kk_device *dev, const void *data,
                             uint32_t size, uint32_t alignment,
                             uint64_t *gpu_address)
{
   assert(data);
   void *cpu;
   VkResult result = kk_heap_metadata_reserve_locked(dev, size, alignment,
                                                     &cpu, gpu_address);
   if (result == VK_SUCCESS)
      memcpy(cpu, data, size);
   return result;
}

VkResult
kk_sampler_params_alloc_locked(struct kk_device *dev,
                              const struct kk_sampler_params *params,
                              uint64_t *gpu_address,
                              const struct kk_sampler_params **cpu_address)
{
   static_assert(sizeof(*params) == KK_SAMPLER_PARAMS_STRIDE,
                 "sampler parameter arena record stride");
   void *cpu;
   uint64_t gpu;
   *gpu_address = 0;
   if (cpu_address)
      *cpu_address = NULL;
   VkResult result = kk_heap_metadata_reserve_locked(dev, sizeof(*params),
      KK_SAMPLER_PARAMS_ALIGNMENT, &cpu, &gpu);
   if (result != VK_SUCCESS)
      return result;
   memcpy(cpu, params, sizeof(*params));
   /* Publish both aliases only after initializing their one immutable record. */
   *gpu_address = gpu;
   if (cpu_address)
      *cpu_address = cpu;
   return VK_SUCCESS;
}

VkResult
kk_sampler_params_alloc(struct kk_device *dev,
                       const struct kk_sampler_params *params,
                       uint64_t *gpu_address,
                       const struct kk_sampler_params **cpu_address)
{
   simple_mtx_lock(&dev->descriptor_heap_lock);
   VkResult result = kk_sampler_params_alloc_locked(dev, params, gpu_address,
                                                     cpu_address);
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return result;
}

#endif
