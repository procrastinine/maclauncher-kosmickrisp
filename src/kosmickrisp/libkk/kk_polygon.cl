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

static global struct poly_geometry_capture *
polygon_capture(global struct poly_geometry_params *p)
{
   return (global struct poly_geometry_capture *)p->capture;
}

static void
polygon_grid(global uint *grid, uint x, uint y)
{
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

KERNEL(1)
libkk_polygon_capture_setup(global struct poly_geometry_params *geometry,
                            global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint streams = geometry->input_primitives * p->streams;
   uint vertices = streams * p->vertices_out;
   uint triangles = streams * sub_sat(p->vertices_out, 2u);
   p->invocations = geometry->input_primitives;
   p->vertices = (uintptr_t)poly_heap_alloc(heap, vertices * p->vertex_stride, 16);
   p->strips = (uintptr_t)poly_heap_alloc(heap, vertices * 16, 16);
   p->counts = (uintptr_t)poly_heap_alloc(heap, streams * 4, 4);
   p->primitive_counts = (uintptr_t)poly_heap_alloc(heap, streams * 4, 4);
   p->triangles = (uintptr_t)poly_heap_alloc(heap, triangles * 16, 16);
   p->clipped = (uintptr_t)poly_heap_alloc(heap, triangles * 8, 8);
   p->clip_counts = (uintptr_t)poly_heap_alloc(heap, triangles * 4, 4);
   polygon_grid(p->capture_grid, p->vertices_out * p->streams, p->invocations);
   polygon_grid(p->clip_grid, triangles, 1);
   p->draw[0] = 0;
   p->draw[1] = 1;
   p->draw[2] = 0;
   p->draw[3] = 0;
}

static bool
polygon_triangle(global struct poly_geometry_capture *p, uint id,
                  uint *invocation, uint *stream, uint4 *vertices)
{
   uint max_primitives = sub_sat(p->vertices_out, 2u);
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

KERNEL(64)
libkk_polygon_clip(global struct poly_geometry_params *geometry,
                   global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   global uint *counts = (global uint *)p->clip_counts;
   global uint64_t *clipped = (global uint64_t *)p->clipped;
   counts[id] = 0;
   clipped[id] = 0;
   uint invocation, stream;
   uint4 vertices;
   if (!polygon_triangle(p, id, &invocation, &stream, &vertices))
      return;

   float positions[3][4];
   float clip[3][KK_POLYGON_CLIP_DISTANCES];
   float cull[3][KK_POLYGON_CLIP_DISTANCES];
   for (uint i = 0; i < 3; ++i) {
      float4 position = polygon_position(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < 4; ++c)
         positions[i][c] = position[c];
      global uint4 *input = polygon_vertex(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < p->clip_count; ++c) {
         float4 value = as_float4(input[polygon_slot(p, VARYING_SLOT_CLIP_DIST0 + c / 4)]);
         clip[i][c] = value[c % 4];
      }
      for (uint c = 0; c < p->cull_count; ++c) {
         float4 value = as_float4(input[polygon_slot(p, VARYING_SLOT_CULL_DIST0 + c / 4)]);
         cull[i][c] = value[c % 4];
      }
   }
   struct kk_polygon_vertex output[KK_POLYGON_MAX_VERTICES];
   uint count = kk_polygon_clip(positions, clip, cull, p->clip_count, p->cull_count,
                                p->depth_clip, p->negative_one_to_one, output);
   if (count < 3)
      return;

   uint provoking = p->provoking_last ? 2 : 0;
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
   bool front = p->front_ccw ? area > 0 : area < 0;
   if ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2)))
      return;

   /* Count first, then prefix sum. Output vertices stay in primitive order
    * without allocating a worst-case varying packet for every clip plane. */
   global struct kk_polygon_vertex *saved = poly_heap_alloc(
      heap, count * sizeof(struct kk_polygon_vertex), 4);
   for (uint i = 0; i < count; ++i)
      saved[i] = output[i];
   clipped[id] = (uintptr_t)saved | (front ? 1ul : 0ul);
   counts[id] = p->polygon_mode == 2 ? count :
      p->polygon_mode == 1 ? count * 2 : (count - 2) * 3;
}

KERNEL(1024)
libkk_polygon_prefix_sum(global struct poly_geometry_params *geometry)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   local uint scratch[32];
   poly_prefix_sum(scratch, (global uint *)p->clip_counts, p->clip_grid[0],
                    1, 0, 1024);
}

KERNEL(1)
libkk_polygon_output_setup(global struct poly_geometry_params *geometry,
                           global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint triangles = p->clip_grid[0];
   uint count = triangles ? ((global uint *)p->clip_counts)[triangles - 1] : 0;
   /* Extra final slot carries the original polygon's facing to the FS. */
   p->output = (uintptr_t)poly_heap_alloc(heap, count * (p->vertex_stride + 16), 16);
   p->draw[0] = count * (p->polygon_mode == 2 ? 6 : 1);
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
   for (uint i = 1; i < count; ++i) {
      struct kk_polygon_vertex b = polygon[i];
      maximum_z = fmax(maximum_z, fabs(polygon_screen_z(p, viewport, &b)));
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

KERNEL(64)
libkk_polygon_output(global struct poly_geometry_params *geometry)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   global uint *counts = (global uint *)p->clip_counts;
   uint first = id ? counts[id - 1] : 0;
   uint output_count = counts[id] - first;
   uint count = p->polygon_mode == 2 ? output_count :
      p->polygon_mode == 1 ? output_count / 2 : output_count / 3 + 2;
   if (!output_count)
      return;
   uint invocation, stream;
   uint4 vertices;
   if (!polygon_triangle(p, id, &invocation, &stream, &vertices))
      return;
   uint64_t clipped = ((global uint64_t *)p->clipped)[id];
   bool front = clipped & 1;
   global struct kk_polygon_vertex *polygon =
      (global struct kk_polygon_vertex *)(clipped & ~1ul);
   float positions[3][4];
   for (uint i = 0; i < 3; ++i) {
      float4 pos = polygon_position(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < 4; ++c)
         positions[i][c] = pos[c];
   }
   uint provoking = p->provoking_last ? 2 : 0;
   global uint4 *provoking_output = polygon_vertex(p, invocation, stream, vertices[provoking]);
   uint viewport = (p->outputs & (1ul << VARYING_SLOT_VIEWPORT)) ?
      provoking_output[polygon_slot(p, VARYING_SLOT_VIEWPORT)].x : 0;
   float bias = polygon_depth_bias(p, viewport, polygon, count);
   for (uint i = 0; i < output_count; ++i) {
      /* Triangle fan or boundary line list. No internal fan edges are drawn
       * in LINE mode, and original flat/provoking values remain unchanged. */
      uint vertex_index = p->polygon_mode == 2 ? i :
         p->polygon_mode == 1 ? (i / 2 + i % 2) % count :
         i % 3 == 0 ? 0 : i / 3 + i % 3;
      struct kk_polygon_vertex vertex = polygon[vertex_index];
      global uint4 *output = (global uint4 *)(p->output +
         (uint64_t)(first + i) * (p->vertex_stride + 16));
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
      output[packed] = (uint4)(front, 0, 0, 0);
   }
}
