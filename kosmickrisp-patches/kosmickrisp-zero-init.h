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

static inline bool
kk_zero_init_optimized_layout(bool optimized, VkImageLayout initial_layout)
{
   /* Zero backing is not a valid Apple lossless-compression encoding for
    * every format. Keep these explicitly initialized images in Metal's
    * unoptimized layout, so every texel and mip preserves its zero bits.
    * Calculate requirements before binding; ordinary images are unchanged.
    */
   return optimized && initial_layout != VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT;
}
#endif
