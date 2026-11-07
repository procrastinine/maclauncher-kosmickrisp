/* SPDX-License-Identifier: MIT */
#ifndef KK_AS_DESCRIPTOR_H
#define KK_AS_DESCRIPTOR_H
#include <stdint.h>
#include <stddef.h>

/* The size is retained at full width, but Vulkan permits zero and forbids
 * using this optional metadata to bound acceleration-structure traversal. */
struct kk_as_descriptor {
   uint64_t address;
   uint64_t size;
};
_Static_assert(sizeof(struct kk_as_descriptor) == 16, "AS descriptor size");
_Static_assert(offsetof(struct kk_as_descriptor, address) == 0, "AS address offset");
_Static_assert(offsetof(struct kk_as_descriptor, size) == 8, "AS range offset");
_Static_assert(__alignof__(struct kk_as_descriptor) == 8, "AS heap address alignment");

static inline struct kk_as_descriptor
kk_as_descriptor_from_range(uint64_t address, uint64_t size)
{
   return (struct kk_as_descriptor){address, address ? size : 0};
}
#endif
