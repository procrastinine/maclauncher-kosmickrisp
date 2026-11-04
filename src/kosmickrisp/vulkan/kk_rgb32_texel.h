/* SPDX-License-Identifier: MIT */
#ifndef KK_RGB32_TEXEL_H
#define KK_RGB32_TEXEL_H

#include "kk_private.h"
#include "kosmickrisp/bridge/mtl_format.h"
#include "kosmickrisp/bridge/mtl_types.h"
#include "util/format/u_format.h"

/* The backing buffer may also be used for writes through other APIs. Its
 * texel-buffer usage must be read-only, since one Vulkan view owns one Metal
 * texture used by both uniform and storage descriptors. Sparse tile aliases
 * retain the existing scalar representation. Family and OS are cached. */
static inline enum mtl_pixel_format
kk_native_rgb32_texel_format(enum pipe_format format, uint32_t apple_family,
                             enum mtl_language_version msl_version,
                             VkBufferUsageFlags2KHR usage,
                             VkBufferCreateFlags flags, VkDeviceSize offset,
                             VkDeviceSize buffer_offset,
                             const uint32_t alignment[3],
                             uint32_t *prefix_B)
{
   if (apple_family < 11 || msl_version < MTL_LANGUAGE_VERSION_4_1 ||
       !(usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT) ||
       (usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) ||
       (flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT))
      return MTL_PIXEL_FORMAT_INVALID;

   enum mtl_pixel_format native;
   switch (format) {
   case PIPE_FORMAT_R32G32B32_FLOAT: native = MTL_PIXEL_FORMAT_R32G32B32_FLOAT; break;
   case PIPE_FORMAT_R32G32B32_UINT: native = MTL_PIXEL_FORMAT_R32G32B32_UINT; break;
   case PIPE_FORMAT_R32G32B32_SINT: native = MTL_PIXEL_FORMAT_R32G32B32_SINT; break;
   default: return MTL_PIXEL_FORMAT_INVALID;
   }

   const uint32_t align = alignment[native - MTL_PIXEL_FORMAT_R32G32B32_UINT];
   if (!align)
      return MTL_PIXEL_FORMAT_INVALID;

   /* Respect Metal's actual format alignment and the buffer's binding offset.
    * Prefixes are whole RGB texels, within both the Vulkan buffer and the
    * headroom reserved by maxTexelBufferElements. Other views stay scalar. */
   for (uint32_t prefix = 0; prefix < 12 * KK_MIN_TEXEL_BUFFER_ALIGNMENT;
        prefix += 12) {
      if (offset >= prefix && (buffer_offset + offset - prefix) % align == 0) {
         *prefix_B = prefix;
         return native;
      }
   }
   return MTL_PIXEL_FORMAT_INVALID;
}
#endif
