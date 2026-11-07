/* SPDX-License-Identifier: MIT */
#ifndef KK_AS_LAYOUT_H
#define KK_AS_LAYOUT_H

#include <stdbool.h>
#include <string.h>
#include "kosmickrisp-as-types.h"

struct kk_as_layout {
   struct kk_as_header header;
   uint64_t scratch_size;
};

static inline bool
kk_as_u64_add(uint64_t a, uint64_t b, uint64_t *result)
{
   if (b > UINT64_MAX - a)
      return false;
   *result = a + b;
   return true;
}

static inline bool
kk_as_u64_mul(uint64_t a, uint64_t b, uint64_t *result)
{
   if (a && b > UINT64_MAX / a)
      return false;
   *result = a * b;
   return true;
}

static inline bool
kk_as_u64_align(uint64_t a, uint64_t alignment, uint64_t *result)
{
   uint64_t end;
   if (!alignment || (alignment & (alignment - 1)) ||
       !kk_as_u64_add(a, alignment - 1, &end))
      return false;
   *result = end & ~(alignment - 1);
   return true;
}

static inline bool
kk_as_append(uint64_t *cursor, uint64_t count, uint64_t size, uint64_t alignment,
             uint64_t *offset)
{
   uint64_t start, bytes, end;
   if (!kk_as_u64_align(*cursor, alignment, &start) ||
       !kk_as_u64_mul(count, size, &bytes) ||
       !kk_as_u64_add(start, bytes, &end))
      return false;
   *offset = start;
   *cursor = end;
   return true;
}

/* All callers use checked intervals, including a view whose size covers the
 * remaining tail of its VkBuffer. Creating a view does not reserve that tail. */
static inline bool
kk_as_range_fits(uint64_t offset, uint64_t size, uint64_t capacity)
{
   return offset <= capacity && size <= capacity - offset;
}

static inline bool
kk_as_address_range_fits(uint64_t address, uint64_t size)
{
   return size <= UINT64_MAX - address;
}

/* The returned grid covers count elements without a narrowing ceil/product.
 * Split over all three dimensions; x/y remain within65535 workgroups. */
static inline bool
kk_as_grid(uint64_t count, uint32_t grid[3])
{
   if (count > KK_AS_MAX_DISPATCH_INVOCATIONS)
      return false;
   grid[0] = count < 65536 ? (uint32_t)count : 65536;
   uint64_t rows = count ? count / grid[0] + (count % grid[0] != 0) : 1;
   grid[1] = rows < 65535 ? (uint32_t)rows : 65535;
   uint64_t plane = (uint64_t)grid[0] * grid[1];
   grid[2] = plane ? (uint32_t)(count / plane + (count % plane != 0)) : 1;
   return true;
}

static inline bool
kk_as_compute_layout(uint32_t type, uint32_t flags, uint64_t geometries,
                     uint64_t primitives, struct kk_as_layout *result)
{
   if (type > KK_AS_TYPE_BOTTOM_LEVEL || geometries > KK_AS_MAX_GEOMETRIES ||
       primitives > KK_AS_MAX_PRIMITIVES ||
       (type == KK_AS_TYPE_TOP_LEVEL &&
        (geometries != 1 || primitives > KK_AS_MAX_INSTANCES)))
      return false;

   struct kk_as_layout out = {0};
   struct kk_as_header *h = &out.header;
   h->magic = KK_AS_MAGIC;
   h->abi_version = KK_AS_ABI_VERSION;
   h->type = type;
   h->status = KK_AS_STATUS_COMPLETE;
   h->build_flags = flags;
   h->primitive_count = primitives;
   h->node_count = primitives ? primitives * 2 - 1 : 0;
   h->geometry_count = geometries;
   h->instance_count = type == KK_AS_TYPE_TOP_LEVEL ? primitives : 0;
   h->bottom_level_pointer_count = h->instance_count;

   uint64_t cursor = sizeof(*h), nodes, ignored;
   if (!kk_as_append(&cursor, h->node_count, sizeof(struct kk_as_node),
                     KK_AS_ALIGNMENT, &nodes) ||
       !kk_as_append(&cursor, geometries, sizeof(struct kk_as_geometry),
                     KK_AS_ALIGNMENT, &h->geometry_offset) ||
       !kk_as_append(&cursor, primitives, sizeof(struct kk_as_payload),
                     KK_AS_ALIGNMENT, &h->payload_offset) ||
       !kk_as_u64_align(cursor, KK_AS_ALIGNMENT, &h->canonical_size))
      return false;
   h->root_offset = primitives ? nodes : 0;
   /* Leaves follow internal nodes. TLAS leaves preserve one pointer per input,
    * including inactive/null instances, in the original instance order. */
   h->instance_offset = h->instance_count ?
      nodes + (primitives - 1) * sizeof(struct kk_as_node) : 0;

   cursor = KK_AS_SERIAL_PREFIX_SIZE;
   if (!kk_as_append(&cursor, h->instance_count, sizeof(uint64_t), 8, &ignored) ||
       !kk_as_u64_align(cursor, KK_AS_ALIGNMENT, &cursor) ||
       !kk_as_u64_add(cursor, h->canonical_size, &h->serialization_size))
      return false;

   /* Construction stages the entire converted representation before the final
    * copy. No original input pointer survives in the completed representation. */
   out.scratch_size = h->canonical_size;
   *result = out;
   return true;
}
#endif
