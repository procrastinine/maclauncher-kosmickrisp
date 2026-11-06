/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SOURCE_DISTANCE_H
#define KK_INTERLOCK_SOURCE_DISTANCE_H

/* Poly captures before the hardware-VS lowering separates merged clip/cull
 * arrays. Other capture producers can already have separate CULL_DIST slots.
 * Keep the original storage/XFB layout and address either representation. */
static float
interlock_source_cull_distance(global struct poly_geometry_capture *p,
   global uint4 *input, uint distance)
{
   bool separate = (p->outputs &
      ((1ul << VARYING_SLOT_CULL_DIST0) | (1ul << VARYING_SLOT_CULL_DIST1))) != 0;
   uint index = separate ? distance : p->clip_count + distance;
   uint slot = (separate ? VARYING_SLOT_CULL_DIST0 : VARYING_SLOT_CLIP_DIST0) + index / 4;
   return as_float4(input[polygon_slot(p, slot)])[index % 4];
}

#endif
