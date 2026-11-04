/*
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT
 */
#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"
#include "poly/geometry_capture.h"
#include "poly/prim.h"
#include "kk_polygon.h"
#include "kk_poly_retained.h"
#include "kk_line_dashes.h"

static global struct poly_geometry_capture *
polygon_capture(global struct poly_geometry_params *p)
{
   return (global struct poly_geometry_capture *)p->capture;
}

static void
polygon_grid(global uint *grid, global uint *groups, uint x, uint y)
{
   groups[0] = x / 64 + (x % 64 != 0);
   groups[1] = y;
   groups[2] = 1;
   grid[0] = x;
   grid[1] = y;
   grid[2] = 1;
   /* Metal4's indirect dispatchThreads record stores threadgroup dimensions,
    * not a threadgroup count. Match KERNEL(64), even for a partial last group. */
   grid[3] = 64;
   grid[4] = 1;
   grid[5] = 1;
}

static global uint4 *
polygon_vertex(global struct poly_geometry_capture *p, uint invocation,
                uint stream, uint vertex)
{
   uint64_t index = ((uint64_t)invocation * p->streams + stream) *
                   p->vertices_out + vertex;
   return (global uint4 *)(p->vertices + index * p->vertex_stride);
}

static uint
polygon_slot(global struct poly_geometry_capture *p, uint location)
{
   return popcount(p->outputs & ((1ul << location) - 1ul));
}

static float4
polygon_position(global struct poly_geometry_capture *p, uint invocation,
                  uint stream, uint vertex)
{
   return as_float4(polygon_vertex(p, invocation, stream, vertex)[
      polygon_slot(p, VARYING_SLOT_POS)]);
}

static void
polygon_capture_setup(global struct poly_geometry_params *geometry,
                       global struct poly_heap *heap, uint arity, bool interlock)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint streams = geometry->input_primitives * p->streams;
   uint vertices = streams * p->vertices_out;
   uint triangles = streams * sub_sat(p->vertices_out, arity - 1u);
   p->invocations = geometry->input_primitives;
   p->dispatch_groups[0] = geometry->grid[0] / 64 + (geometry->grid[0] % 64 != 0);
   p->dispatch_groups[1] = geometry->grid[1];
   p->dispatch_groups[2] = 1;
   p->line_phases = p->line_stipple_flags && p->primitive_vertices == 2 ?
      (uintptr_t)poly_heap_alloc(heap, triangles * 8, 8) : 0;
   p->vertices = (uintptr_t)poly_heap_alloc(heap, vertices * p->vertex_stride, 16);
   p->strips = (uintptr_t)poly_heap_alloc(heap, vertices * 16, 16);
   p->counts = (uintptr_t)poly_heap_alloc(heap, streams * 4, 4);
   p->primitive_counts = (uintptr_t)poly_heap_alloc(heap, streams * 4, 4);
   p->triangles = (uintptr_t)poly_heap_alloc(heap, triangles * 16, 16);
   uint clipped_size = triangles * 8;
   if (interlock && arity < 3)
      clipped_size = align(clipped_size, 16u) +
         triangles * arity * sizeof(struct kk_polygon_vertex);
   p->clipped = (uintptr_t)poly_heap_alloc(heap, clipped_size, interlock && arity < 3 ? 16 : 8);
   p->clip_counts = (uintptr_t)poly_heap_alloc(heap, triangles * 4, 4);
   polygon_grid(p->capture_grid, p->capture_groups, p->vertices_out * p->streams, p->invocations);
   polygon_grid(p->clip_grid, p->clip_groups, triangles, 1);
   p->draw[0] = 0;
   p->draw[1] = 1;
   p->draw[2] = 0;
   p->draw[3] = 0;
}

KERNEL(1)
libkk_polygon_capture_setup(global struct poly_geometry_params *geometry,
                            global struct poly_heap *heap)
{
   polygon_capture_setup(geometry, heap,
      polygon_capture(geometry)->primitive_vertices, false);
}

static bool
polygon_triangle(global struct poly_geometry_capture *p, uint id,
                  uint *invocation, uint *stream, uint4 *vertices)
{
   uint max_primitives = sub_sat(p->vertices_out, p->primitive_vertices - 1u);
   uint stream_index = id / max_primitives;
   uint primitive = id % max_primitives;
   *stream = stream_index % p->streams;
   *invocation = stream_index / p->streams;
   global uint *counts = (global uint *)p->primitive_counts;
   if (primitive >= counts[stream_index] || *stream != p->rasterization_stream)
      return false;
   *vertices = ((global uint4 *)p->triangles)[id];
   return true;
}

/* Reject from original vertices before loading positions or clip distances.
 * Test one plane at a time: a nonnegative or NaN vertex keeps that plane.
 * Points/lines read only their real vertices, not the padded triangle packet. */
static bool
polygon_cull_primitive(global struct poly_geometry_capture *p,
                       uint invocation, uint stream, uint4 vertices)
{
   for (uint c = 0; c < p->cull_count; ++c) {
      uint slot = polygon_slot(p, VARYING_SLOT_CULL_DIST0 + c / 4);
      bool culled = true;
      for (uint v = 0; v < p->primitive_vertices; ++v) {
         global uint4 *input = polygon_vertex(p, invocation, stream, vertices[v]);
         if (!(as_float4(input[slot])[c % 4] < 0.0f)) {
            culled = false;
            break;
         }
      }
      if (culled)
         return true;
   }
   return false;
}

static void
polygon_clip_primitive(global struct poly_geometry_params *geometry,
                       global struct poly_heap *heap, uint id, bool defer_facing)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   global uint *counts = (global uint *)p->clip_counts;
   global uint64_t *clipped = (global uint64_t *)p->clipped;
   if (id >= p->clip_grid[0]) return;
   counts[id] = 0;
   clipped[id] = 0;
   if (p->line_phases) ((global uint64_t *)p->line_phases)[id] = 0;
   uint invocation, stream;
   uint4 vertices;
   if (!polygon_triangle(p, id, &invocation, &stream, &vertices))
      return;

   if (polygon_cull_primitive(p, invocation, stream, vertices))
      return;

   float positions[3][4];
   float clip[3][KK_POLYGON_CLIP_DISTANCES];
   for (uint i = 0; i < 3; ++i) {
      float4 position = polygon_position(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < 4; ++c)
         positions[i][c] = position[c];
      global uint4 *input = polygon_vertex(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < p->clip_count; ++c) {
         float4 value = as_float4(input[polygon_slot(p, VARYING_SLOT_CLIP_DIST0 + c / 4)]);
         clip[i][c] = value[c % 4];
      }
   }
   struct kk_polygon_vertex output[KK_POLYGON_MAX_VERTICES];
   uint count = p->primitive_vertices == 1 ?
      kk_polygon_clip_point(positions, clip, clip, p->clip_count, 0,
                            p->depth_clip, p->negative_one_to_one, output) :
      p->primitive_vertices == 2 ?
      kk_polygon_clip_line(positions, clip, clip, p->clip_count, 0,
                           p->depth_clip, p->negative_one_to_one, output) :
      kk_polygon_clip(positions, clip, clip, p->clip_count, 0,
                      p->depth_clip, p->negative_one_to_one, output);
   if (count < p->primitive_vertices)
      return;

   uint provoking = p->provoking_last ? p->primitive_vertices - 1 : 0;
   global uint4 *provoking_output = polygon_vertex(p, invocation, stream, vertices[provoking]);
   uint viewport = (p->outputs & (1ul << VARYING_SLOT_VIEWPORT)) ?
      provoking_output[polygon_slot(p, VARYING_SLOT_VIEWPORT)].x : 0;
   float area = 0;
   for (uint i = 0; i < count; ++i) {
      uint next = (i + 1) % count;
      float w = output[i].position[3], next_w = output[next].position[3];
      if (w == 0 || next_w == 0)
         continue;
      area += (output[i].position[0] * output[next].position[1] -
               output[next].position[0] * output[i].position[1]) / (w * next_w);
   }
   /* Vulkan's signed framebuffer area has a leading minus sign. */
   area *= -p->viewport[viewport][2] * p->viewport[viewport][3];
   bool front = p->primitive_vertices <= 2 || (p->front_ccw ? area > 0 : area < 0);
   if (p->primitive_vertices > 2 && !defer_facing &&
       ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2))))
      return;

   /* Count first, then prefix sum. Output vertices stay in primitive order
    * without allocating a worst-case varying packet for every clip plane. */
   uint saved_bytes = count * sizeof(struct kk_polygon_vertex);
   bool geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   if (geometry_stipple) saved_bytes = align(saved_bytes, 8) + 8;
   global struct kk_polygon_vertex *saved = poly_heap_alloc(
      heap, saved_bytes, geometry_stipple ? 8 : 4);
   if (geometry_stipple) {
      global uint4 *primitives = (global uint4 *)p->triangles;
      /* GS vertices_out is 16-bit; the upper word is free strip metadata. */
      uint4 primitive = primitives[id];
      primitive.w = (primitive.w & 0xffffu) | (count << 16);
      primitives[id] = primitive;
   }
   for (uint i = 0; i < count; ++i)
      saved[i] = output[i];
   clipped[id] = (uintptr_t)saved | (front ? 1ul : 0ul);
   if (p->line_phases) {
      float2 a = (float2)(output[0].position[0], output[0].position[1]) / output[0].position[3];
      float2 b = (float2)(output[1].position[0], output[1].position[1]) / output[1].position[3];
      float2 d = (b - a) * (float2)(p->viewport[viewport][2], p->viewport[viewport][3]) * 0.5f;
      float length = p->line_mode == 2 ? fmax(fabs(d.x), fabs(d.y)) : kk_line_length(d);
      ((global uint64_t *)p->line_phases)[id] =
         p->line_mode == 2 ?
            kk_line_steps(kk_line_screen(p, viewport, &output[0]),
                          kk_line_screen(p, viewport, &output[1]), p->line_mode, p->line_width) :
            (uint64_t)ceil(length);
   }
   counts[id] = p->primitive_vertices <= 2 ? count :
      p->polygon_mode == 2 ? count :
      p->polygon_mode == 1 ? count * 2 : (count - 2) * 3;
}

KERNEL(64)
libkk_polygon_clip(global struct poly_geometry_params *geometry,
                   global struct poly_heap *heap)
{
   polygon_clip_primitive(geometry, heap, cl_global_id.x, false);
}

KERNEL(64)
libkk_conservative_clip(global struct poly_geometry_params *geometry,
                        global struct poly_heap *heap)
{
   polygon_clip_primitive(geometry, heap, cl_global_id.x, true);
}


/* Count merged ON spans after stipple phase is known. Original clipped
 * endpoints and weights stay in place, and the shared count prefix/output
 * kernels compact only the visible spans. */
/* Variable-length dash walks are independent per original primitive. */
KERNEL(1)
libkk_polygon_dash_counts(global struct poly_geometry_params *geometry,
                           global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   if (id >= p->clip_grid[0]) return;
   global uint *counts = (global uint *)p->clip_counts;
   counts[id] = 0;
   uint64_t clipped = ((global uint64_t *)p->clipped)[id];
   if (!clipped) return;
   uint invocation, stream;
   uint4 vertices;
   if (!polygon_triangle(p, id, &invocation, &stream, &vertices)) return;
   uint original_count = vertices.w >> 16;
   if (original_count < p->primitive_vertices) return;
   global struct kk_polygon_vertex *polygon =
      (global struct kk_polygon_vertex *)(clipped & ~1ul);
   *kk_line_dash_pointer(polygon, original_count) = 0;
   uint provoking = p->provoking_last ? p->primitive_vertices - 1 : 0;
   global uint4 *pv = polygon_vertex(p, invocation, stream, vertices[provoking]);
   uint viewport = p->outputs & (1ul << VARYING_SLOT_VIEWPORT) ?
      pv[polygon_slot(p, VARYING_SLOT_VIEWPORT)].x : 0;
   uint edges = p->primitive_vertices == 2 ? 1 : original_count;
   uint period = max(p->line_stipple_factor, 1u) * 16u;
   uint64_t initial_phase = kk_line_primitive_phase(geometry, p, id, invocation, stream, vertices) % period;
   uint64_t phase = initial_phase;
   uint total = 0;
   for (uint edge = 0; edge < edges; ++edge) {
      float2 a = kk_line_screen(p, viewport, &polygon[edge]);
      float2 b = kk_line_screen(p, viewport, &polygon[(edge + 1) % original_count]);
      total += kk_line_geometry_dashes(p, a, b, p->line_stipple_pattern,
         p->line_stipple_factor, (uint)(phase % period), edge, NULL);
      phase += kk_line_steps(a, b, p->line_mode, p->line_width);
   }
   if (!total) return;
   global struct kk_line_dash *spans = poly_heap_alloc(heap,
      total * sizeof(struct kk_line_dash), 16);
   *kk_line_dash_pointer(polygon, original_count) = (uintptr_t)spans;
   phase = initial_phase;
   uint written = 0;
   for (uint edge = 0; edge < edges; ++edge) {
      float2 a = kk_line_screen(p, viewport, &polygon[edge]);
      float2 b = kk_line_screen(p, viewport, &polygon[(edge + 1) % original_count]);
      written += kk_line_geometry_dashes(p, a, b, p->line_stipple_pattern,
         p->line_stipple_factor, (uint)(phase % period), edge, spans + written);
      phase += kk_line_steps(a, b, p->line_mode, p->line_width);
   }
   counts[id] = written * (p->line_mode == 2 ? 2u : 4u);
}

KERNEL(1024)
libkk_polygon_prefix_sum(global struct poly_geometry_params *geometry)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   local uint scratch[32];
   poly_prefix_sum(scratch, (global uint *)p->clip_counts, p->clip_grid[0],
                    1, 0, 1024);
   if (p->line_phases && !(p->line_stipple_flags & KK_LINE_STIPPLE_PHASE_READY)) {
      global uint64_t *phase = (global uint64_t *)p->line_phases;
      uint tid = cl_local_id.x;
      uint64_t carry = 0;
      for (uint base = 0; base < p->clip_grid[0]; base += 1024) {
         uint64_t value = base + tid < p->clip_grid[0] ? phase[base + tid] : 0;
         /* Reuse the original 32-subgroup scratch. A low-word prefix wraps
          * exactly when its result is less than this element's low word. */
         uint2 low = poly_work_group_scan_inclusive_add((uint)value, scratch);
         uint2 high = poly_work_group_scan_inclusive_add(
            (uint)(value >> 32) + (uint)(low.x < (uint)value), scratch);
         uint64_t prefix = ((uint64_t)high.x << 32) | low.x;
         uint64_t total = ((uint64_t)high.y << 32) | low.y;
         if (base + tid < p->clip_grid[0]) phase[base + tid] = carry + prefix;
         carry += total;
      }
      if (cl_local_id.x == 0) p->line_stipple_flags |= KK_LINE_STIPPLE_PHASE_READY;
   }
}

KERNEL(1)
libkk_polygon_output_setup(global struct poly_geometry_params *geometry,
                           global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint triangles = p->clip_grid[0];
   uint count = triangles ? ((global uint *)p->clip_counts)[triangles - 1] : 0;
   /* Extra final slot carries the original polygon's facing to the FS. */
   bool geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   uint packet_stride = p->vertex_stride + 16 + (geometry_stipple ? 16 : 0);
   p->output = (uintptr_t)poly_heap_alloc(heap, count * packet_stride, 16);
   p->draw[0] = kk_poly_retained_vertex(count, p->polygon_mode, p->line_expand,
      p->line_mode, p->line_width, geometry_stipple, false);
}

static float
polygon_screen_z(global struct poly_geometry_capture *p, uint viewport,
                  const struct kk_polygon_vertex *vertex)
{
   float z = vertex->position[2] / vertex->position[3];
   if (p->negative_one_to_one)
      z = 0.5f * (z + 1.0f);
   float minimum = p->viewport[viewport][4];
   float maximum = p->viewport[viewport][5];
   return z * (maximum - minimum) + minimum;
}

static float
polygon_depth_bias_units(global struct poly_geometry_capture *p,
                          float maximum_z, float slope)
{
   int exponent = p->depth_is_float ?
      max((int)((as_uint(maximum_z) >> 23) & 255), 1) - 127 : 0;
   int power = exponent - (int)p->depth_mantissa_bits;
   /* FORCE_UNORM uses mantissa bits plus one on floating-point attachments.
    * FLOAT uses one unit of framebuffer depth. Vulkan enum values are 1/2. */
   if (p->depth_bias_representation == 1)
      power = -(int)(p->depth_mantissa_bits + p->depth_is_float);
   else if (p->depth_bias_representation == 2)
      power = 0;
   /* Exact powers of two, including FP32 subnormal spacing, without a libcl
    * math-library call in these standalone precompiled Mesa kernels. */
   uint resolvable_bits = power >= -126 ? (uint)(power + 127) << 23
                                         : 1u << (power + 149);
   float resolvable = as_float(resolvable_bits);
   float bias = p->depth_bias_constant * resolvable + p->depth_bias_slope * slope;
   if (p->depth_bias_clamp > 0)
      bias = fmin(bias, p->depth_bias_clamp);
   else if (p->depth_bias_clamp < 0)
      bias = fmax(bias, p->depth_bias_clamp);
   return bias;
}

static float
polygon_depth_bias(global struct poly_geometry_capture *p, uint viewport,
                    global struct kk_polygon_vertex *polygon, uint count)
{
   if (!p->depth_bias_enable)
      return 0;
   float slope = 0;
   float maximum_z = 0;
   struct kk_polygon_vertex a = polygon[0];
   float ax = a.position[0] / a.position[3];
   float ay = a.position[1] / a.position[3];
   float az = polygon_screen_z(p, viewport, &a);
   maximum_z = fabs(az);
   /* Only floating-point FORMAT spacing depends on the clipped-depth
    * exponent. FLOAT, FORCE_UNORM and fixed-point FORMAT do not use it. */
   if (p->depth_is_float && p->depth_bias_representation == 0) {
      for (uint i = 1; i < count; ++i) {
         struct kk_polygon_vertex b = polygon[i];
         maximum_z = fmax(maximum_z, fabs(polygon_screen_z(p, viewport, &b)));
      }
   }
   for (uint i = 1; i + 1 < count; ++i) {
      struct kk_polygon_vertex b = polygon[i], c = polygon[i + 1];
      float bx = (b.position[0] / b.position[3] - ax) * p->viewport[viewport][2] * 0.5f;
      float by = (b.position[1] / b.position[3] - ay) * p->viewport[viewport][3] * 0.5f;
      float cx = (c.position[0] / c.position[3] - ax) * p->viewport[viewport][2] * 0.5f;
      float cy = (c.position[1] / c.position[3] - ay) * p->viewport[viewport][3] * 0.5f;
      float bz = polygon_screen_z(p, viewport, &b) - az;
      float cz = polygon_screen_z(p, viewport, &c) - az;
      float determinant = bx * cy - by * cx;
      if (determinant != 0) {
         slope = fmax(fabs((bz * cy - cz * by) / determinant),
                      fabs((bx * cz - cx * bz) / determinant));
         break;
      }
   }
   return polygon_depth_bias_units(p, maximum_z, slope);
}

KERNEL(64)
libkk_polygon_output(global struct poly_geometry_params *geometry)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   if (id >= p->clip_grid[0]) return;
   global uint *counts = (global uint *)p->clip_counts;
   uint first = id ? counts[id - 1] : 0;
   uint output_count = counts[id] - first;
   uint count = p->primitive_vertices <= 2 ? output_count :
      p->polygon_mode == 2 ? output_count :
      p->polygon_mode == 1 ? output_count / 2 : output_count / 3 + 2;
   if (!output_count)
      return;
   uint invocation, stream;
   uint4 vertices;
   if (!polygon_triangle(p, id, &invocation, &stream, &vertices))
      return;
   uint64_t clipped = ((global uint64_t *)p->clipped)[id];
   bool front = clipped & 1;
   bool geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   if (geometry_stipple) count = vertices.w >> 16;
   global struct kk_polygon_vertex *polygon =
      (global struct kk_polygon_vertex *)(clipped & ~1ul);
   float positions[3][4];
   for (uint i = 0; i < 3; ++i) {
      float4 pos = polygon_position(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < 4; ++c)
         positions[i][c] = pos[c];
   }
   uint provoking = p->provoking_last ? p->primitive_vertices - 1 : 0;
   global uint4 *provoking_output = polygon_vertex(p, invocation, stream, vertices[provoking]);
   uint viewport = (p->outputs & (1ul << VARYING_SLOT_VIEWPORT)) ?
      provoking_output[polygon_slot(p, VARYING_SLOT_VIEWPORT)].x : 0;
   float bias = p->primitive_vertices == 2 ? 0 : polygon_depth_bias(p, viewport, polygon, count);
   uint64_t phase = 0;
   if (p->line_phases) {
      uint maximum = sub_sat(p->vertices_out, 1u);
      uint first_primitive = (invocation * p->streams + stream) * maximum + (vertices.w & 0xffffu);
      if (p->line_strip) {
         uint per_instance = max(geometry->grid[0], 1u);
         uint instance_first = invocation - invocation % per_instance;
         uint strip_first = p->restart_strips ? ((global uint *)p->restart_strips)[invocation % per_instance] : 0;
         first_primitive = (instance_first + strip_first) * p->streams * maximum;
      }
      global uint64_t *phases = (global uint64_t *)p->line_phases;
      phase = (id ? phases[id - 1] : 0) - (first_primitive ? phases[first_primitive - 1] : 0);
      phase %= max(p->line_stipple_factor, 1u) * 16u;
   }
   float bres_origin = 0;
   if (p->line_stipple_flags && p->primitive_vertices == 2 && p->line_expand && p->line_mode == 2) {
      float2 a = kk_line_screen(p, viewport, &polygon[0]);
      float2 b = kk_line_screen(p, viewport, &polygon[1]);
      int4 bounds = kk_line_wide_bres_bounds(a, b, p->line_width);
      bres_origin = bounds.z * ((float)bounds.x - floor(bounds.w ? a.x : a.y));
   }
   uint packet_stride = p->vertex_stride + 16 + (geometry_stipple ? 16 : 0);
   float polygon_phase = 0;
   for (uint i = 0; i < output_count; ++i) {
      /* Triangle fan or boundary line list. No internal fan edges are drawn
       * in LINE mode, and original flat/provoking values remain unchanged. */
      uint vertex_index = p->primitive_vertices <= 2 ? i :
         p->polygon_mode == 2 ? i :
         p->polygon_mode == 1 ? (i / 2 + i % 2) % count :
         i % 3 == 0 ? 0 : i / 3 + i % 3;
      struct kk_polygon_vertex vertex;
      float original_along = 0, original_length = 0;
      float4 original_endpoints = (float4)(0);
      if (geometry_stipple) {
         global struct kk_line_dash *spans =
            (global struct kk_line_dash *)*kk_line_dash_pointer(polygon, count);
         struct kk_line_dash span = spans[i / (p->line_mode == 2 ? 2u : 4u)];
         struct kk_polygon_vertex a = polygon[span.edge];
         struct kk_polygon_vertex b = polygon[(span.edge + 1) % count];
         float t = (i & 1u) ? span.last : span.first;
         float2 screen_a = kk_line_screen(p, viewport, &a);
         float2 screen_b = kk_line_screen(p, viewport, &b);
         float2 delta = screen_b - screen_a;
         original_length = kk_line_length(delta);
         original_along = p->line_mode == 2 ? span.rectangle.x : 0;
         original_endpoints = (float4)(screen_a, screen_b);
         float2 rectangle_point = (float2)(0);
         if (p->line_mode != 2) {
            uint corner = i & 3u;
            rectangle_point = (float2)((corner & 2u) ? span.rectangle.z : span.rectangle.x,
                                        (corner & 1u) ? span.rectangle.w : span.rectangle.y);
            float along = ((rectangle_point.x - screen_a.x) * delta.x +
                           (rectangle_point.y - screen_a.y) * delta.y) / original_length;
            float cap = p->line_mode == 3 ? 0.5f : 0;
            t = (along + cap) / (original_length + 2 * cap);
         }
         phase = (uint64_t)span.phase;
         /* Screen-linear cuts require homogeneous interpolation of clip
          * coordinates and the original triangle's interpolation weights. */
         float homogeneous = t * a.position[3] /
            ((1.0f - t) * b.position[3] + t * a.position[3]);
         vertex = kk_polygon_intersection(&a, &b, homogeneous);
         if (p->line_mode != 2) {
            vertex.position[0] = ((rectangle_point.x - p->viewport[viewport][0]) *
               (2.0f / p->viewport[viewport][2]) - 1.0f) * vertex.position[3];
            vertex.position[1] = ((rectangle_point.y - p->viewport[viewport][1]) *
               (2.0f / p->viewport[viewport][3]) - 1.0f) * vertex.position[3];
         }
      } else {
         vertex = polygon[vertex_index];
      }
      global uint4 *output = (global uint4 *)(p->output +
         (uint64_t)(first + i) * packet_stride);
      uint packed = 0;
      for (uint slot = 0; slot < 64; ++slot) {
         if (!(p->outputs & (1ul << slot)))
            continue;
         uint4 value = polygon_vertex(p, invocation, stream, vertices[provoking])[packed];
         if ((p->float_outputs & (1ul << slot)) &&
             !(geometry->flat_outputs & (1ul << slot))) {
            float4 interpolated;
            for (uint c = 0; c < 4; ++c) {
               float input[3];
               for (uint v = 0; v < 3; ++v)
                  input[v] = as_float4(polygon_vertex(p, invocation, stream, vertices[v])[packed])[c];
               interpolated[c] = kk_polygon_varying(&vertex, positions, input,
                  p->noperspective_outputs & (1ul << slot));
            }
            value = as_uint4(interpolated);
         }
         if (slot == VARYING_SLOT_POS) {
            float z = polygon_screen_z(p, viewport, &vertex) + bias;
            if (p->depth_clamp) {
               float minimum = p->viewport[viewport][4], maximum = p->viewport[viewport][5];
               if (p->polygon_mode == 2)
                  z = clamp(z, fmin(minimum, maximum), fmax(minimum, maximum));
               else
                  /* Native clamp follows interpolation, including a polygon
                   * crossing the viewport depth bound after applying bias. */
                  z = minimum == maximum ? 0 : (z - minimum) / (maximum - minimum);
            }
            float4 position = (float4)(vertex.position[0], vertex.position[1],
                                       z * vertex.position[3], vertex.position[3]);
            value = as_uint4(position);
         }
         output[packed++] = value;
      }
      if (!geometry_stipple && p->line_stipple_flags && p->line_expand && p->primitive_vertices == 3 && (i & 1u)) {
         struct kk_polygon_vertex a = polygon[(i / 2u) % count];
         struct kk_polygon_vertex b = polygon[(i / 2u + 1u) % count];
         float2 d = ((float2)(b.position[0], b.position[1]) / b.position[3] -
                     (float2)(a.position[0], a.position[1]) / a.position[3]) *
                    (float2)(p->viewport[viewport][2], p->viewport[viewport][3]) * 0.5f;
         polygon_phase += p->line_mode == 2 ? ceil(fmax(fabs(d.x), fabs(d.y))) : ceil(sqrt(d.x * d.x + d.y * d.y));
      }
      if (!geometry_stipple && p->line_stipple_flags && p->primitive_vertices == 3 &&
          p->line_expand && p->line_mode == 2) {
         float2 a = kk_line_screen(p, viewport, &polygon[(i / 2u) % count]);
         float2 b = kk_line_screen(p, viewport, &polygon[(i / 2u + 1u) % count]);
         int4 bounds = kk_line_wide_bres_bounds(a, b, p->line_width);
         bres_origin = bounds.z * ((float)bounds.x - floor(bounds.w ? a.x : a.y));
      }
      output[packed] = (uint4)(front, as_uint((float)phase + polygon_phase - bres_origin),
         as_uint(original_along), as_uint(original_length));
      if (geometry_stipple) output[packed + 1] = as_uint4(original_endpoints);
   }
}

/* GPU scheduling can select an original assembled primitive or an explicit
 * batch. The inclusive prefix keeps every clipped/expanded subdivision in one
 * draw; the original linked PrimitiveID/Layer/rate remain in the packets. */
KERNEL(1)
libkk_polygon_retained_draw(global struct poly_geometry_params *geometry,
   constant struct kk_poly_retained_range *range, global uint *draw,
   uint conservative)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint first = min(range->first, p->clip_grid[0]);
   uint end = first + min(range->count, p->clip_grid[0] - first);
   global uint *counts = (global uint *)p->clip_counts;
   uint a = first ? counts[first - 1] : 0;
   uint b = end ? counts[end - 1] : 0;
   uint geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   uint start = kk_poly_retained_vertex(a, p->polygon_mode, p->line_expand,
      p->line_mode, p->line_width, geometry_stipple, conservative);
   uint stop = kk_poly_retained_vertex(b, p->polygon_mode, p->line_expand,
      p->line_mode, p->line_width, geometry_stipple, conservative);
   draw[0] = stop - start; draw[1] = 1; draw[2] = start; draw[3] = 0;
}

#include "kk_conservative.cl"

#include "kk_interlock_source.cl"
