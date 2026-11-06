/* SPDX-License-Identifier: MIT */
#ifndef KK_CANONICAL_LOCATION_LAYOUT_H
#define KK_CANONICAL_LOCATION_LAYOUT_H

#include "compiler/shader_enums.h"

#define KK_CANONICAL_VARYING_COUNT \
   (VARYING_SLOT_VAR31 - VARYING_SLOT_VAR0 + 1)

/* The projected form shares the packet, adding native homogeneous corner
 * coordinates and one ordinary interpolation carrier. These locations belong
 * to the canonical ABI and are independent of the producer stage. */
#define KK_CANONICAL_PROJECTED_POINT (VARYING_SLOT_VAR0 + 1)
static inline unsigned
kk_canonical_position_offset(unsigned corner)
{
   return (3u * KK_CANONICAL_VARYING_COUNT + corner) * 16u;
}

static inline unsigned
kk_canonical_projected_buffer_size(void)
{
   return (3u * KK_CANONICAL_VARYING_COUNT + 3u) * 16u;
}

static inline unsigned
kk_canonical_buffer_offset(unsigned location, unsigned corner)
{
   return (corner * KK_CANONICAL_VARYING_COUNT +
           location - VARYING_SLOT_VAR0) * 16u;
}

static inline unsigned
kk_canonical_buffer_size(void)
{
   return 3u * KK_CANONICAL_VARYING_COUNT * 16u;
}

#endif
