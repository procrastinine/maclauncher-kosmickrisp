/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_RETAINED_H
#define KK_POLY_RETAINED_H
#include "compiler/libcl/libcl.h"
struct kk_poly_retained_range { uint32_t first, count; };

/* Same packet-to-native-vertex mapping as polygon_output_setup. Prefix values
 * are packet offsets, including all subdivisions of one original primitive. */
static inline uint32_t
kk_poly_retained_vertex(uint32_t packets, uint32_t polygon_mode,
   uint32_t line_expand, uint32_t line_mode, float line_width,
   uint32_t geometry_stipple, uint32_t conservative)
{
   if (conservative) return packets * 6u;
   uint32_t line_vertices = line_mode == 2 ?
      2u * (uint32_t)(line_width + 0.5f) : 6u;
   if (geometry_stipple && line_mode == 2) line_vertices = 2;
   return geometry_stipple && line_mode != 2 ? packets / 4u * 6u :
      line_expand ? packets / 2u * line_vertices :
      packets * (polygon_mode == 2 ? 6u : 1u);
}
#endif
