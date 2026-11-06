/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT */
#pragma once

#include "compiler/libcl/libcl.h"

/* Per-draw storage for polygon rasterization after geometry assembly. */
struct poly_geometry_capture {
   uint64_t vertices;
   uint64_t strips;
   uint64_t counts;
   uint64_t primitive_counts;
   uint64_t triangles;
   uint64_t clipped;
   uint64_t clip_counts;
   uint64_t output;
   uint64_t outputs;
   uint64_t float_outputs;
   uint64_t noperspective_outputs;
   uint32_t vertex_stride;
   uint32_t vertices_out;
   uint32_t streams;
   uint32_t invocations;
   uint32_t clip_count;
   uint32_t cull_count;
   uint32_t cull_mode;
   uint32_t front_ccw;
   uint32_t depth_clip;
   uint32_t depth_clamp;
   uint32_t negative_one_to_one;
   uint32_t provoking_last;
   uint32_t rasterization_stream;
   uint32_t capture_grid[6];
   uint32_t clip_grid[6];
   uint32_t draw[4];
   float viewport[16][6];
   float render_size[2];
   float depth_bias_constant;
   float depth_bias_slope;
   float depth_bias_clamp;
   uint32_t depth_mantissa_bits;
   uint32_t depth_is_float;
   uint32_t depth_bias_enable;
};

#ifndef __OPENCL_VERSION__
struct poly_gs_capture_info {
   uint64_t outputs;
   uint64_t float_outputs;
   uint32_t vertices_out;
   uint32_t streams;
   uint8_t clip_count;
   uint8_t cull_count;
   uint16_t output_types[64];
   uint8_t output_components[64];
};
#endif
