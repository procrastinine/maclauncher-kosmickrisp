/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_ENVELOPE_H
#define KK_INTERLOCK_ENVELOPE_H

#include <stdint.h>
#include "poly/geometry_capture.h"

struct kk_interlock_stencil_state {
   uint32_t reference;
   uint32_t compare_mask;
   uint32_t write_mask;
   uint32_t compare;
   uint32_t fail;
   uint32_t depth_fail;
   uint32_t pass;
};

/* The existing Poly capture pointer addresses this prefix. Capture and replay
 * consume the same geometry; application vertex/geometry programs run once. */
struct kk_interlock_envelope {
   struct poly_geometry_capture geometry;
   uint64_t inputs;
   uint64_t claims;
   uint64_t coverage;
   uint64_t metadata;
   uint64_t sample_outputs;
   uint64_t depth;
   uint64_t stencil;
   uint64_t color[8];
   uint64_t occlusion;
   uint32_t width;
   uint32_t height;
   uint32_t tile_x;
   uint32_t tile_y;
   uint32_t tile_width;
   uint32_t tile_height;
   uint32_t samples;
   uint32_t layer;
   uint32_t primitive_base;
   uint32_t primitive_count;
   uint32_t input_stride;
   uint32_t output_stride;
   uint32_t sample_mask;
   float sample_positions[32][2];
   uint32_t depth_enable;
   uint32_t depth_write;
   uint32_t depth_compare;
   uint32_t bounds_enable;
   uint32_t stencil_enable;
   float bounds_min;
   float bounds_max;
   struct kk_interlock_stencil_state stencil_state[2];
   uint64_t color_texture[8];
   uint64_t depth_texture;
   uint64_t stencil_texture;
   /* Each source segment executes once per required Vulkan view. */
   uint32_t source_vertices_per_primitive;
   uint32_t view_mask;
   uint32_t source_view;
   uint32_t source_view_ordinal;
   uint32_t source_points_expanded;
   /* Group-indirect arguments; clip_grid keeps the logical primitive count. */
   uint32_t source_dispatch[3];
};

#endif
