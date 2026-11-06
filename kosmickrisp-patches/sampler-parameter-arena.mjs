// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const id = 'sampler-parameter-arena';
export const sourceFiles = [
  'kosmickrisp-patches/sampler-params-api.h',
  'kosmickrisp-patches/sampler-params-impl.h',
];

export function apply({edit, edits}) {
  for (const [input, output] of [
    ['sampler-params-api.h', 'kk_sampler_params_api.h'],
    ['sampler-params-impl.h', 'kk_sampler_params_impl.h'],
  ]) {
    edits.set('src/kosmickrisp/vulkan/' + output,
      fs.readFileSync(new URL(input, import.meta.url), 'utf8'));
  }

  edit('src/kosmickrisp/vulkan/kk_sampler.h', s => s
    .replace('#include "vk_sampler.h"',
      '#include "vk_sampler.h"\n#include "kk_sampler_params_api.h"')
    .replace('   const VkSamplerCreateInfo *info, VkSampler *sampler);',
      '   const VkSamplerCreateInfo *info, VkSampler *sampler,\n   enum kk_sampler_params_lock_context params_lock_context);'));
  edit('src/kosmickrisp/vulkan/kk_sampler.c', s => s
    .replace('                  bool native_only)\n{',
      '                  bool native_only,\n                  enum kk_sampler_params_lock_context params_lock_context)\n{')
    .replace('   *pSampler = kk_sampler_to_handle(sampler);',
      `   /* The composed sampler finalizer receives explicit caller context. */
   assert(params_lock_context == KK_SAMPLER_PARAMS_UNLOCKED ||
          params_lock_context == KK_SAMPLER_PARAMS_HEAP_LOCK_HELD);
   (void)params_lock_context;
   *pSampler = kk_sampler_to_handle(sampler);`)
    .replace('   return kk_create_sampler(device, pCreateInfo, pAllocator, pSampler, false);',
      '   return kk_create_sampler(device, pCreateInfo, pAllocator, pSampler, false,\n                            KK_SAMPLER_PARAMS_UNLOCKED);')
    .replace('                        VkSampler *sampler)\n{',
      '                        VkSampler *sampler,\n                        enum kk_sampler_params_lock_context params_lock_context)\n{')
    .replace('   return kk_create_sampler(kk_device_to_handle(dev), info, NULL, sampler, true);',
      '   return kk_create_sampler(kk_device_to_handle(dev), info, NULL, sampler, true,\n                            params_lock_context);'));

  edit('src/kosmickrisp/vulkan/kk_descriptor_heap_api.h', s => s
    .replace('   VkResult result = kk_create_heap_sampler(dev, &info, &entry->sampler);',
      '   VkResult result = kk_create_heap_sampler(dev, &info, &entry->sampler,\n                                             KK_SAMPLER_PARAMS_HEAP_LOCK_HELD);')
    .replace('static VkResult\nkk_heap_sampler_metadata_locked(',
      '#include "kk_sampler_params_impl.h"\n\nstatic VkResult\nkk_heap_sampler_metadata_locked(')
    .replace(`   struct kk_heap_metadata_page *page = dev->heap_metadata_pages;
   uint32_t offset = page ? ALIGN_POT(page->used, 16) : 0;
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
   memcpy((uint8_t *)page->bo->cpu + offset, entry->descriptor, size);
   entry->gpu_address = page->bo->gpu + offset;
   page->used = offset + size;
   return VK_SUCCESS;`,
      `   return kk_heap_metadata_alloc_locked(dev, entry->descriptor, size, 16,
                                        &entry->gpu_address);`));
}
