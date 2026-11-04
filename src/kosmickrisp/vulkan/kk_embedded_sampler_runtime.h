/* SPDX-License-Identifier: MIT */
#ifndef KK_EMBEDDED_SAMPLER_RUNTIME_H
#define KK_EMBEDDED_SAMPLER_RUNTIME_H
#include "vk_sampler.h"

VkResult
kk_shader_embedded_samplers_init(struct kk_device *dev, struct kk_shader *shader,
                                uint32_t count,
                                const void *state_bytes)
{
   if (!count)
      return VK_SUCCESS;
   if (!state_bytes || count > kk_device_physical(dev)->vk.properties.maxDescriptorHeapEmbeddedSamplers)
      return vk_error(dev, VK_ERROR_UNKNOWN);
   if (shader->embedded_sampler_count) {
      if (shader->embedded_sampler_count != count ||
          memcmp(shader->embedded_sampler_states, state_bytes,
                 count * sizeof(struct vk_sampler_state)))
         return vk_error(dev, VK_ERROR_UNKNOWN);
      return VK_SUCCESS;
   }
   struct vk_sampler_state *copy = vk_alloc(&dev->vk.alloc,
      count * sizeof(*copy), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!copy)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   memcpy(copy, state_bytes, count * sizeof(*copy));
   uint64_t table = 0;
   VkResult result = kk_heap_embedded_sampler_table(dev, count, copy, &table);
   if (result != VK_SUCCESS) {
      vk_free(&dev->vk.alloc, copy);
      return result;
   }
   shader->embedded_sampler_states = copy;
   shader->embedded_sampler_count = count;
   shader->embedded_sampler_table = table;
   return VK_SUCCESS;
}

void
kk_shader_embedded_samplers_finish(struct kk_device *dev,
                                  struct kk_shader *shader)
{
   vk_free(&dev->vk.alloc, shader->embedded_sampler_states);
   shader->embedded_sampler_states = NULL;
   shader->embedded_sampler_count = 0;
   shader->embedded_sampler_table = 0;
}
#endif
