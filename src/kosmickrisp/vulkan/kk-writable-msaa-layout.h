/* SPDX-License-Identifier: MIT */
#ifndef KK_WRITABLE_MSAA_LAYOUT_H
#define KK_WRITABLE_MSAA_LAYOUT_H
#include <stddef.h>
#include <stdint.h>
#include "util/macros.h"

#define KK_WRITABLE_MSAA_ENTRY_SIZE_B 8u
struct kk_writable_msaa_defaults {
   uint32_t storage_alpha_one;
   uint32_t sampled_default_one_mask;
};
static_assert(sizeof(struct kk_writable_msaa_defaults) == 8, "MS defaults size");
static_assert(offsetof(struct kk_writable_msaa_defaults, storage_alpha_one) == 0,
              "MS storage default offset");
static_assert(offsetof(struct kk_writable_msaa_defaults, sampled_default_one_mask) == 4,
              "MS sampled default offset");

static inline uint64_t
kk_writable_msaa_defaults_offset_B(uint32_t samples)
{
   return (uint64_t)samples * KK_WRITABLE_MSAA_ENTRY_SIZE_B;
}
static inline uint64_t
kk_writable_msaa_table_size_B(uint32_t samples)
{
   return kk_writable_msaa_defaults_offset_B(samples) +
          sizeof(struct kk_writable_msaa_defaults);
}
#endif
