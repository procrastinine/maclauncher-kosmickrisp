/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_RASTER_TOPOLOGY_H
#define KK_POLY_RASTER_TOPOLOGY_H
#include <math.h>
#include "util/macros.h"

/* Shared by the capture upload and statically specialized raster reader. */
static inline float
kk_polygon_line_width(float width)
{
   return CLAMP(floorf(width * 256.0f + 0.5f) / 256.0f, 1.0f, 4096.0f);
}

/* The cached reader emits native points for original point primitives. Line
 * and polygon-line readers use native Bresenham lines or expanded triangles;
 * polygon points are expanded squares, not native point primitives. */
static inline enum mtl_primitive_type
kk_polygon_raster_primitive(unsigned primitive_vertices, VkPolygonMode mode,
                             VkLineRasterizationMode line_mode)
{
   if (primitive_vertices == 1)
      return MTL_PRIMITIVE_TYPE_POINT;
   if ((primitive_vertices == 2 || mode == VK_POLYGON_MODE_LINE) &&
       line_mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM)
      return MTL_PRIMITIVE_TYPE_LINE;
   return MTL_PRIMITIVE_TYPE_TRIANGLE;
}

static inline enum mtl_primitive_topology_class
kk_polygon_raster_topology(unsigned primitive_vertices, VkPolygonMode mode,
                            VkLineRasterizationMode line_mode)
{
   switch (kk_polygon_raster_primitive(primitive_vertices, mode, line_mode)) {
   case MTL_PRIMITIVE_TYPE_POINT: return MTL_PRIMITIVE_TOPOLOGY_CLASS_POINT;
   case MTL_PRIMITIVE_TYPE_LINE: return MTL_PRIMITIVE_TOPOLOGY_CLASS_LINE;
   default: return MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE;
   }
}
#endif
