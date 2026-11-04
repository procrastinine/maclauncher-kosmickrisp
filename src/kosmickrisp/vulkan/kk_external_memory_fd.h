/* SPDX-License-Identifier: MIT */
#ifndef KK_EXTERNAL_MEMORY_FD_H
#define KK_EXTERNAL_MEMORY_FD_H

#include "kk_format.h"
#include "vk_format.h"
#include "util/format/u_format.h"

static inline bool
kk_fd_image_supported(VkFormat format, VkImageType type, VkImageTiling tiling,
                      VkImageUsageFlags2KHR usage, VkImageCreateFlags2KHR flags)
{
   const VkImageCreateFlags2KHR allowed = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
      VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_ALIAS_BIT;
   if (type != VK_IMAGE_TYPE_2D ||
       (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR) ||
       (flags & ~allowed) || (usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ||
       vk_format_is_compressed(format) || vk_format_is_depth_or_stencil(format) ||
       vk_format_get_ycbcr_info(format) != NULL ||
       vk_format_get_plane_count(format) != 1)
      return false;

   enum pipe_format pf = vk_format_to_pipe_format(format);
   if (pf == PIPE_FORMAT_NONE || kk_format_is_rgb32(pf) ||
       !util_is_power_of_two_nonzero(util_format_get_blocksize(pf)))
      return false;

   /* Native ordinary/packed color formats; no emulated byte representation. */
   const struct kk_va_format *native = kk_get_va_format(pf);
   return native && native->is_native;
}

#endif /* KK_EXTERNAL_MEMORY_FD_H */
