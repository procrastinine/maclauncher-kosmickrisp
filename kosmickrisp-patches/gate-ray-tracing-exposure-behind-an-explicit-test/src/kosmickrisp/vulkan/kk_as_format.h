/* SPDX-License-Identifier: MIT */
#ifndef KK_AS_FORMAT_H
#define KK_AS_FORMAT_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#include "kosmickrisp/as/kosmickrisp-as-types.h"

/* The AS builder's vertex decoder and format advertisement share this map. */
static inline bool
kk_as_vertex_format(VkFormat format, uint32_t *decoded)
{
   uint32_t value;
   switch (format) {
#define KK_AS_FORMAT(vk, as) case VK_FORMAT_##vk: value = KK_AS_##as; break
   KK_AS_FORMAT(R32G32_SFLOAT, R32G32_SFLOAT);
   KK_AS_FORMAT(R32G32B32_SFLOAT, R32G32B32_SFLOAT);
   KK_AS_FORMAT(R16G16_SFLOAT, R16G16_SFLOAT);
   KK_AS_FORMAT(R16G16_SNORM, R16G16_SNORM);
   KK_AS_FORMAT(R16G16B16A16_SFLOAT, R16G16B16A16_SFLOAT);
   KK_AS_FORMAT(R16G16B16A16_SNORM, R16G16B16A16_SNORM);
   KK_AS_FORMAT(R16G16B16A16_UNORM, R16G16B16A16_UNORM);
   KK_AS_FORMAT(R16G16_UNORM, R16G16_UNORM);
   KK_AS_FORMAT(A2B10G10R10_UNORM_PACK32, A2B10G10R10_UNORM);
   KK_AS_FORMAT(R8G8B8A8_UNORM, R8G8B8A8_UNORM);
   KK_AS_FORMAT(R8G8_UNORM, R8G8_UNORM);
   KK_AS_FORMAT(R8G8B8A8_SNORM, R8G8B8A8_SNORM);
   KK_AS_FORMAT(R8G8_SNORM, R8G8_SNORM);
#undef KK_AS_FORMAT
   default: return false;
   }
   if (decoded)
      *decoded = value;
   return true;
}

#endif
