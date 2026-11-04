/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT */
#pragma once
#include "poly/geometry_capture.h"
#include "kk_polygon.h"

#define KK_CONSERVATIVE_UNCERTAINTY (1.0f / 256.0f)
#define KK_CONSERVATIVE_ADDRESS_SLOT VARYING_SLOT_VAR0

/* Only conservative draws allocate this extension of the existing capture. */
struct kk_conservative_params {
   struct poly_geometry_capture polygon;
   float extra_size;
   uint32_t samples;
   uint32_t sample_mask;
   uint64_t canonical_locations;
   uint64_t canonical_provoking_components[2];
   uint32_t inner_coverage;
};

/* Original corner values use the same 16-byte-per-location heap transport as
 * PerVertexKHR. A flat address keeps full interfaces within Metal's IO limit. */
struct kk_conservative_primitive {
   uint64_t corners[3];
   uint64_t canonical;
   uint64_t polygon;
   uint32_t count;
   uint32_t provoking;
   uint32_t front;
   uint32_t front_ccw;
   uint32_t layer;
   uint32_t viewport_index;
   uint32_t depth_clamp;
   uint32_t samples;
   uint32_t sample_mask;
   float viewport[6];
   float render_size[2];
   float bounds[4];
   float positions[3][4]; /* homogeneous window X, Y, Z, clip W */
   float clip_bounds[4];
   float edges[KK_POLYGON_MAX_VERTICES][4];
   float bias;
   float extra_size;
   uint32_t degenerate;
   uint64_t inner_edges;
};

struct kk_conservative_inner_edge {
   int64_t a, b, c;
};

struct kk_conservative_span {
   uint64_t primitive;
   float bounds[4];
};
