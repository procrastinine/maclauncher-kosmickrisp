/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_PARAMS_API_H
#define KK_SAMPLER_PARAMS_API_H

#include "kk_descriptor_types.h"
#include "kosmickrisp/compiler/kk-sampler-params.h"
#include "vulkan/vulkan_core.h"

struct kk_device;

/* The locked entry requires descriptor_heap_lock. The wrapper acquires it.
 * Both retain immutable records until valid device teardown. Neither entry
 * may be called while the native sampler mutex is held. Optional cpu_address
 * aliases the same initialized bytes as gpu_address; neither owns another copy. */
VkResult kk_sampler_params_alloc_locked(struct kk_device *dev,
   const struct kk_sampler_params *params, uint64_t *gpu_address,
   const struct kk_sampler_params **cpu_address);
VkResult kk_sampler_params_alloc(struct kk_device *dev,
   const struct kk_sampler_params *params, uint64_t *gpu_address,
   const struct kk_sampler_params **cpu_address);

static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_lod_bias_fp16) == 12,
              "sampler parameter address chunk0");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_lod_min_fp16) == 14,
              "sampler parameter address chunk1");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16) == 16,
              "sampler parameter address chunk2");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode) == 22,
              "sampler parameter address chunk3");

/* Apply after ordinary sampler assignments on native and virtual records.
 * Bit4 identifies these pointer words independently of bit3 sampling dispatch.
 * Untagged native records retain numeric half-float decoding. Image, border,
 * plane-chain and auxiliary fields are untouched. */
static inline void
kk_sampler_params_pack_gpu_address(struct kk_sampled_image_descriptor *dst,
                                  uint64_t gpu_address)
{
   dst->sampler_lod_bias_fp16 = (uint16_t)gpu_address;
   dst->sampler_lod_min_fp16 = (uint16_t)(gpu_address >> 16);
   dst->sampler_lod_max_fp16 = (uint16_t)(gpu_address >> 32);
   dst->sampler_mipmap_mode = (uint16_t)(gpu_address >> 48);
   /* Publish presence only after every unsigned pointer chunk is complete. */
   dst->sampler_filter_bits |= KK_SAMPLER_FILTER_PARAMS_POINTER;
}

#endif
