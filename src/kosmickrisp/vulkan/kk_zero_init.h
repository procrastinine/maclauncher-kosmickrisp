/* SPDX-License-Identifier: MIT */
#ifndef KK_ZERO_INIT_H
#define KK_ZERO_INIT_H

#include "kk_image_layout.h"
#include "vk_extensions.h"
#include "vk_physical_device_features.h"

static inline void
kk_zero_init_features(struct vk_device_extension_table *extensions,
                      struct vk_features *features)
{
   extensions->EXT_zero_initialize_device_memory = true;
   features->zeroInitializeDeviceMemory = true;
}

/* Block-compressed zero blocks remain in their raw native encoding. Apple
 * does not apply its separate lossless texture compression to these formats.
 * Other optimal images keep their ordinary optimized layout and are initialized
 * through typed native operations at the initial layout transition.
 */
static inline bool
kk_zero_init_optimized_layout(bool optimized, VkImageLayout initial_layout,
                              bool block_compressed)
{
   return optimized && !(initial_layout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT &&
                          block_compressed);
}

#define KK_ZERO_IMAGE_UPLOAD_SIZE (64 * 1024)

/* Every tile fits the bounded immutable zero upload, including edge blocks. */
static inline VkExtent2D
kk_zero_init_tile(uint32_t width, uint32_t height, uint32_t block_width,
                   uint32_t block_height, uint32_t block_bytes)
{
   uint32_t columns = MIN2(DIV_ROUND_UP(width, block_width),
                           KK_ZERO_IMAGE_UPLOAD_SIZE / block_bytes);
   uint32_t rows = MIN2(DIV_ROUND_UP(height, block_height),
                        KK_ZERO_IMAGE_UPLOAD_SIZE / (columns * block_bytes));
   return (VkExtent2D){MIN2(width, columns * block_width),
                       MIN2(height, rows * block_height)};
}
#endif
