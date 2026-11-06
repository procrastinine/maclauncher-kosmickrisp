/* SPDX-License-Identifier: MIT */
#pragma once

/* Raw component selection preserves NaN payloads and signed zeros in the
 * explicit per-vertex components while replacing only ordinary attributes. */
static inline uint4
kk_conservative_provoking_components(uint4 value, uint4 provoking, uint mask)
{
   uint4 bits;
   bits[0] = 0u - (mask & 1u);
   bits[1] = 0u - ((mask >> 1) & 1u);
   bits[2] = 0u - ((mask >> 2) & 1u);
   bits[3] = 0u - ((mask >> 3) & 1u);
   return (value & ~bits) | (provoking & bits);
}
