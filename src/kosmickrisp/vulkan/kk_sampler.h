/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_SAMPLER_H
#define KK_SAMPLER_H 1

#include "kk_device.h"
#include "kk_physical_device.h"
#include "kk_private.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_sampler.h"
#include "kk_sampler_params_api.h"
#include "vk_ycbcr_conversion.h"

#include "vk_format.h"

struct kk_sampler {
   struct vk_sampler vk;
   VkClearColorValue custom_border;
   bool has_border;
   bool native_only;

   uint8_t plane_count;
   uint16_t lod_bias_fp16;
   uint16_t lod_min_fp16;
   uint16_t lod_max_fp16;
   uint16_t reduction_mode;
   uint16_t filter_bits;
   uint16_t address_modes;
   uint16_t query_lod_bias_fp16;
   uint16_t mipmap_mode;

   struct {
      struct kk_rc_sampler *hw;
   } planes[2];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

VkResult kk_create_heap_sampler(struct kk_device *dev,
   const VkSamplerCreateInfo *info, VkSampler *sampler,
   enum kk_sampler_params_lock_context params_lock_context);

#endif /* KK_SAMPLER_H */
