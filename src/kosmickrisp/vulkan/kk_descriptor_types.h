/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */
#ifndef KK_DESCRIPTOR_TYPES
#define KK_DESCRIPTOR_TYPES 1

#include "kk_private.h"
#include <stddef.h>
#include "util/format/u_format.h"

/* TODO_KOSMICKRISP Reduce size to 32 bytes by moving border to a heap. */
struct kk_sampled_image_descriptor {
   uint64_t image_gpu_resource_id;
   uint16_t sampler_index;

   /* Negative if there is no border color, else the clamp=0 sampler index used
    * for custom border color emulation.
    */
   int16_t clamp_0_sampler_index_or_negative;

   uint16_t sampler_lod_bias_fp16;
   uint16_t sampler_lod_min_fp16;
   uint16_t sampler_lod_max_fp16;

   uint16_t image_min_lod_fp16;
   uint16_t image_min_lod_uint16;

   uint16_t sampler_mipmap_mode;
   uint32_t border[4];
   uint16_t sampler_query_lod_bias_fp16;
   uint16_t sampler_reduction_mode;
   uint16_t sampler_filter_bits;
   uint16_t sampler_address_modes;
   uint64_t aux_resource_id;
   uint64_t aux_word;
};

#define KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE BITFIELD_BIT(2)
static_assert((KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE & 3u) == 0,
              "cube flag does not overlap the two linear-filter bits");
static_assert(KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE <= UINT16_MAX,
              "cube flag fits existing sampler filter metadata");
static_assert(KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE ==
              VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT,
              "sampler cube flag retains its Vulkan bit encoding");

static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) == 18,
              "image minimum LOD retains its upstream field");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) == 20,
              "integer image minimum LOD retains its upstream field");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode) == 22,
              "mipmap mode occupies the upstream alignment padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_query_lod_bias_fp16) == 40,
              "query bias occupies the upstream tail padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_reduction_mode) == 42,
              "sampler reduction occupies descriptor padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits) == 44,
              "sampler filters occupy descriptor padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_address_modes) == 46,
              "sampler addressing occupies descriptor padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode) == 22,
              "mipmap metadata ABI");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_query_lod_bias_fp16) == 40,
              "sampler bias metadata ABI");
static_assert(offsetof(struct kk_sampled_image_descriptor, aux_resource_id) == 48,
              "sampled image auxiliary resource ID offset");
static_assert(offsetof(struct kk_sampled_image_descriptor, aux_word) == 56,
              "sampled image auxiliary word offset");
static_assert(sizeof(struct kk_sampled_image_descriptor) == 64,
              "kk_sampled_image_descriptor has no holes");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) ==
              offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16) +
              sizeof(uint16_t), "image min LOD follows sampler LOD");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) ==
              offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) +
              sizeof(uint16_t), "image min LOD fields are adjacent");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) +
              sizeof(uint16_t) <=
              offsetof(struct kk_sampled_image_descriptor, border),
              "image min LOD does not overlap trailing descriptor fields");

#define KK_TEXEL_BUFFER_RGB32 (1u << 31)
/* Buffer records only: word0 is a byte address, word8 holds this tag and an
 * existing pipe_format, and word12 is the logical element count. Native image
 * array IDs use a different view class and never consume this interpretation. */
#define KK_TEXEL_BUFFER_RAW_ADDRESS (1u << 30)
#define KK_TEXEL_BUFFER_FORMAT_MASK (KK_TEXEL_BUFFER_RAW_ADDRESS - 1u)
static_assert(PIPE_FORMAT_COUNT <= KK_TEXEL_BUFFER_FORMAT_MASK,
              "raw texel format fits the common buffer payload");
static_assert((KK_TEXEL_BUFFER_RAW_ADDRESS & KK_TEXEL_BUFFER_RGB32) == 0,
              "raw buffer mode and native RGB32 remain independent");

struct kk_storage_image_descriptor {
   uint64_t image_gpu_resource_id;
   union {
      struct { uint32_t texel_offset, texel_count; };
      uint64_t image_array_gpu_resource_id;
   };
};

static_assert(offsetof(struct kk_storage_image_descriptor, image_array_gpu_resource_id) ==
              sizeof(uint64_t),
              "2D array handles and texel/3D metadata occupy disjoint view classes");
static_assert(offsetof(struct kk_sampled_image_descriptor, aux_resource_id) == 48,
              "array aliasing retains the sampled descriptor auxiliary ABI");
static_assert(sizeof(struct kk_storage_image_descriptor) == 16,
              "kk_storage_image_descriptor has no holes");

/* This has to match nir_address_format_64bit_bounded_global */
struct kk_buffer_address {
   uint64_t base_addr;
   uint32_t size;
   uint32_t zero; /* Sparse record in descriptor storage; zero in the lowered address. */
};

static_assert(sizeof(struct kk_buffer_address) == 16,
              "kk_buffer_address has no holes");

#endif /* KK_DESCRIPTOR_TYPES */
