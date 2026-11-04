/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_SAMPLER_H
#define KK_SAMPLER_H 1

#include "kk_device.h"
#include "kk_sampler_emulation.h"
#include "kosmickrisp/compiler/kk-sampler-params.h"
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
   bool is_virtual;
   uint64_t params_gpu_address;
   /* Host alias of the same device-retained immutable arena record. */
   const struct kk_sampler_params *params_cpu;
   uint16_t plane_filter_bits[2];

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

/* Finalized implementation state, not raw filter/LOD values or allocation
 * of a metadata record. Native Apple10 reduction is already encoded in Metal. */
static inline bool
kk_sampler_needs_emulation(const struct kk_sampler *sampler)
{
   return sampler->is_virtual || sampler->has_border || sampler->reduction_mode != 0 ||
          (sampler->filter_bits & KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE);
}

static inline void
kk_sampler_publish_emulation(struct kk_device *dev, const struct kk_sampler *sampler)
{
   if (kk_sampler_needs_emulation(sampler))
      p_atomic_set(&dev->sampler_emulation, 1);
}

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

VkResult kk_create_heap_sampler(struct kk_device *dev,
   const VkSamplerCreateInfo *info, VkSampler *sampler,
   enum kk_sampler_params_lock_context params_lock_context);

/* Immutable after publication; shared cache ownership outlives local RCs. */
struct kk_sampler_native_state {
   struct mtl_sampler_packed key;
   mtl_sampler *handle;
};

VkResult kk_sampler_cap_native_init(struct kk_device *dev);

void kk_sampler_cap_native_finish(struct kk_device *dev);
uint32_t kk_sampler_cap_native_states(struct kk_device *dev,
   const struct kk_sampler_native_state **states);
VkResult kk_sampler_cap_get_canonical(struct kk_device *dev,
   const struct mtl_sampler_packed *key, struct kk_rc_sampler **out, bool native_only);
VkResult kk_sampler_cap_finalize(struct kk_device *dev, struct kk_sampler *sampler,
   const VkSamplerCreateInfo *info, enum kk_sampler_params_lock_context lock_context);

#endif /* KK_SAMPLER_H */
