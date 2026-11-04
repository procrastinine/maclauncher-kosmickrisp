/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_FORMAT_H
#define KK_FORMAT_H 1

#include "kk_private.h"

#include "util/format/u_format.h"

struct kk_physical_device;
enum pipe_format;
enum mtl_pixel_format;

struct kk_va_format {
   /* Transforms from the native format to the emulated format.
    * Would love to use enum pipe_swizzle, but it's bigger than the required
    * type for util_format_compose_swizzles... */
   struct {
      union {
         struct {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t alpha;
         };
         uint8_t channels[4];
      };
   } unswizzle;
   uint32_t mtl_pixel_format;
   uint8_t bit_widths;
   uint8_t filter  : 1;
   uint8_t write   : 1;
   uint8_t color   : 1;
   uint8_t blend   : 1;
   uint8_t msaa    : 1;
   uint8_t resolve : 1;
   uint8_t sparse  : 1;
   uint8_t atomic  : 1;
   struct {
      uint8_t write      : 1;
      uint8_t read       : 1;
      uint8_t read_write : 1;
   } texel_buffer;
   uint8_t is_native : 1;
};

static inline bool
kk_format_is_rgb32(enum pipe_format format)
{
   return format == PIPE_FORMAT_R32G32B32_FLOAT ||
          format == PIPE_FORMAT_R32G32B32_UINT ||
          format == PIPE_FORMAT_R32G32B32_SINT;
}

static inline enum pipe_format
kk_format_backing(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32G32B32_FLOAT: return PIPE_FORMAT_R32G32B32A32_FLOAT;
   case PIPE_FORMAT_R32G32B32_UINT: return PIPE_FORMAT_R32G32B32A32_UINT;
   case PIPE_FORMAT_R32G32B32_SINT: return PIPE_FORMAT_R32G32B32A32_SINT;
   default: return format;
   }
}

const struct kk_va_format *kk_get_va_format(enum pipe_format format);

enum mtl_pixel_format vk_format_to_mtl_pixel_format(enum VkFormat vkformat);

/* Formats handled by Mesa's shared typed pack/unpack operations. R64 integer
 * operations use the existing full-width address path instead. */
static inline bool
kk_raw_texel_format_supported(enum pipe_format format)
{
   if (format == PIPE_FORMAT_R64_UINT || format == PIPE_FORMAT_R64_SINT ||
       format == PIPE_FORMAT_R11G11B10_FLOAT || format == PIPE_FORMAT_R9G9B9E5_FLOAT)
      return true;
   if (format <= PIPE_FORMAT_NONE || format >= PIPE_FORMAT_COUNT)
      return false;
   const struct util_format_description *desc = util_format_description(format);
   if (desc->layout != UTIL_FORMAT_LAYOUT_PLAIN || desc->block.width != 1 ||
       desc->block.height != 1 || desc->block.bits > 128 ||
       (desc->block.bits & 7) || !desc->nr_channels ||
       (desc->colorspace != UTIL_FORMAT_COLORSPACE_RGB &&
        desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB))
      return false;
   for (unsigned c = 0; c < desc->nr_channels; ++c) {
      const struct util_format_channel_description *channel = &desc->channel[c];
      if (channel->size > 32 ||
          (channel->type != UTIL_FORMAT_TYPE_VOID &&
           channel->type != UTIL_FORMAT_TYPE_UNSIGNED &&
           channel->type != UTIL_FORMAT_TYPE_SIGNED &&
           channel->type != UTIL_FORMAT_TYPE_FLOAT) ||
          (channel->type == UTIL_FORMAT_TYPE_FLOAT &&
           channel->size != 16 && channel->size != 32))
         return false;
      if (c && ((desc->block.bits <= 32 && channel->shift !=
                 desc->channel[c - 1].shift + desc->channel[c - 1].size) ||
                (desc->block.bits > 32 && channel->size != desc->channel[0].size)))
         return false;
   }
   return true;
}

#endif /* KK_FORMAT_H */
