/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT
 * Reuses the original Mesa/Poly primitive capture and polygon clipper. */
#include "kk_conservative.h"
#include "kk_conservative_canonical.h"

static float4
conservative_window(float4 position, global const float *vp, bool minus_one)
{
   float inverse_w = 1.0f / position.w;
   float3 ndc = position.xyz * inverse_w;
   float z = minus_one ? 0.5f * (ndc.z + 1.0f) : ndc.z;
   return (float4)(vp[0] + 0.5f * (ndc.x + 1.0f) * vp[2],
                   vp[1] + 0.5f * (ndc.y + 1.0f) * vp[3],
                   vp[4] + z * (vp[5] - vp[4]), inverse_w);
}

#include "kk_conservative_inner.cl"

/* Intersect the square-expanded convex polygon with a row of pixel centers.
 * Emitting whole accepted pixels gives correct coverage before early tests. */
static bool
conservative_span(global struct kk_conservative_primitive *p, uint row,
                   float2 *range)
{
   float y = (float)row + 0.5f;
   float radius = 0.5f + p->extra_size;
   float lo = p->clip_bounds[0] - radius;
   float hi = p->clip_bounds[2] + radius;
   if (y < p->clip_bounds[1] - radius || y > p->clip_bounds[3] + radius)
      return false;
   for (uint i = 0; i < p->count; ++i) {
      float4 edge = vload4(0, p->edges[i]);
      float c = edge.y * y + edge.z + edge.w;
      if (edge.x > 0.0f)
         lo = fmax(lo, -c / edge.x);
      else if (edge.x < 0.0f)
         hi = fmin(hi, -c / edge.x);
      else if (c < 0.0f)
         return false;
   }
   float first = fmax(ceil(lo - 0.5f), 0.0f);
   float last = fmin(floor(hi - 0.5f), p->render_size[0] - 1.0f);
   *range = (float2)(first, last + 1.0f);
   return first <= last;
}

KERNEL(64)
libkk_conservative_prepare(global struct poly_geometry_params *geometry,
                           global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   global struct kk_conservative_params *config =
      (global struct kk_conservative_params *)p;
   uint id = cl_global_id.x;
   if (id >= p->clip_grid[0]) return;
   global uint *counts = (global uint *)p->clip_counts;
   uint written = counts[id];
   counts[id] = 0;
   if (!written)
      return;
   uint count = p->polygon_mode == 2 ? written :
      p->polygon_mode == 1 ? written / 2 : written / 3 + 2;
   uint invocation, stream;
   uint4 vertices;
   if (!polygon_triangle(p, id, &invocation, &stream, &vertices))
      return;
   uint64_t saved = ((global uint64_t *)p->clipped)[id];
   global struct kk_polygon_vertex *polygon =
      (global struct kk_polygon_vertex *)(saved & ~1ul);
   uint provoking = p->provoking_last ? 2 : 0;
   global uint4 *prov = polygon_vertex(p, invocation, stream, vertices[provoking]);
   uint viewport = (p->outputs & (1ul << VARYING_SLOT_VIEWPORT)) ?
      prov[polygon_slot(p, VARYING_SLOT_VIEWPORT)].x : 0;
   global const float *vp = p->viewport[viewport];
   float2 lo = (float2)(INFINITY), hi = (float2)(-INFINITY);
   long area = 0;
   long2 previous = (long2)(0), first = (long2)(0);
   for (uint i = 0; i < count; ++i) {
      float4 pos = vload4(0, polygon[i].position);
      float2 window = conservative_window(pos, vp, p->negative_one_to_one).xy;
      if (!all(isfinite(window))) return;
      lo = fmin(lo, window);
      hi = fmax(hi, window);
      /* Area on the reported 8-bit
       * subpixel grid, after clipping. Translation avoids cancellation. */
      long2 fixed = convert_long2(rint(window * 256.0f));
      if (i == 0)
         first = fixed;
      else
         area += (previous.x - first.x) * (fixed.y - first.y) -
                 (previous.y - first.y) * (fixed.x - first.x);
      previous = fixed;
   }
   bool degenerate = area == 0;
   bool front = !degenerate && (saved & 1);
   if ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2)))
      return;
   if (degenerate) {
      lo = rint(lo * 256.0f) / 256.0f;
      hi = rint(hi * 256.0f) / 256.0f;
   }
   float2 clip_lo = lo, clip_hi = hi;
   float extra = KK_CONSERVATIVE_UNCERTAINTY + config->extra_size;
   lo = fmax(floor(lo - extra), (float2)(0.0f));
   hi = fmin(ceil(hi + extra), vload2(0, p->render_size));
   if (any(hi <= lo))
      return;
   global struct kk_conservative_primitive *out =
      poly_heap_alloc(heap, sizeof(struct kk_conservative_primitive), 8);
   for (uint i = 0; i < 3; ++i) {
      out->corners[i] = (uintptr_t)polygon_vertex(p, invocation, stream, vertices[i]);
      float4 pos = polygon_position(p, invocation, stream, vertices[i]);
      float depth = p->negative_one_to_one ? 0.5f * (pos.z + pos.w) : pos.z;
      float4 window = (float4)(vp[0] * pos.w + 0.5f * (pos.x + pos.w) * vp[2],
                                vp[1] * pos.w + 0.5f * (pos.y + pos.w) * vp[3],
                                vp[4] * pos.w + depth * (vp[5] - vp[4]), pos.w);
      for (uint c = 0; c < 4; ++c)
         out->positions[i][c] = window[c];
   }
   out->polygon = saved & ~1ul;
   out->count = count;
   out->provoking = provoking;
   out->front = front;
   out->degenerate = degenerate;
   out->inner_edges = config->inner_coverage && !degenerate ?
      kk_conservative_build_inner(heap, polygon, count, vp, p->negative_one_to_one) : 0;
   out->front_ccw = p->front_ccw;
   out->viewport_index = viewport;
   out->depth_clamp = p->depth_clamp;
   out->samples = config->samples;
   out->sample_mask = config->sample_mask;
   out->canonical = 0;
   if (config->canonical_locations) {
      const uint slots = VARYING_SLOT_VAR31 - VARYING_SLOT_VAR0 + 1;
      global uint4 *values = poly_heap_alloc(heap, 3 * slots * 16, 16);
      for (uint corner = 0; corner < 3; ++corner) {
         global uint4 *input = polygon_vertex(p, invocation, stream, vertices[corner]);
         /* Keep the canonical ABI stride, but visit only locations the final
          * fragment interface actually consumes. */
         uint locations = config->canonical_locations >> VARYING_SLOT_VAR0;
         while (locations) {
            uint index = ctz(locations);
            uint location = VARYING_SLOT_VAR0 + index;
            locations &= locations - 1;
            uint4 value = p->outputs & (1ul << location) ? input[polygon_slot(p, location)] : (uint4)(0);
            uint mask = (config->canonical_provoking_components[index / 16] >> ((index % 16) * 4)) & 15;
            if (degenerate && mask && (p->outputs & (1ul << location)))
               value = kk_conservative_provoking_components(value, prov[polygon_slot(p, location)], mask);
            values[corner * slots + index] = value;
         }
      }
      out->canonical = (uintptr_t)values;
   }
   out->layer = (p->outputs & (1ul << VARYING_SLOT_LAYER)) ?
      prov[polygon_slot(p, VARYING_SLOT_LAYER)].x : 0;
   for (uint i = 0; i < 6; ++i)
      out->viewport[i] = vp[i];
   for (uint i = 0; i < 2; ++i)
      out->render_size[i] = p->render_size[i];
   out->bounds[0] = lo.x; out->bounds[1] = lo.y;
   out->bounds[2] = hi.x; out->bounds[3] = hi.y;
   out->bias = polygon_depth_bias(p, viewport, polygon, count);
   if (degenerate && p->depth_bias_enable) {
      float4 position = polygon_position(p, invocation, stream, vertices[provoking]);
      float depth = conservative_window(position, vp, p->negative_one_to_one).z;
      out->bias = polygon_depth_bias_units(p, fabs(depth), 0.0f);
   }
   out->extra_size = extra;
   out->clip_bounds[0] = clip_lo.x; out->clip_bounds[1] = clip_lo.y;
   out->clip_bounds[2] = clip_hi.x; out->clip_bounds[3] = clip_hi.y;
   for (uint i = 0; i < count; ++i) {
      /* Walk previous -> current: keep the loop's increment out of the
       * address expressions before the final store. */
      uint previous = i == 0 ? count - 1 : i - 1;
      float2 a = conservative_window(vload4(0, polygon[previous].position), vp,
                                     p->negative_one_to_one).xy;
      float2 b = conservative_window(vload4(0, polygon[i].position),
                                     vp, p->negative_one_to_one).xy;
      if (degenerate) {
         a = rint(a * 256.0f) / 256.0f;
         b = rint(b * 256.0f) / 256.0f;
      }
      float2 edge = (float2)(a.y - b.y, b.x - a.x);
      if (area < 0.0f)
         edge = -edge;
      out->edges[i][0] = edge.x; out->edges[i][1] = edge.y;
      out->edges[i][2] = -dot(edge, a);
      out->edges[i][3] = (0.5f + extra) * (fabs(edge.x) + fabs(edge.y));
   }
   ((global uint64_t *)p->clipped)[id] = (uintptr_t)out;
   uint spans = 0;
   for (uint row = (uint)lo.y; row < (uint)hi.y; ++row) {
      float2 range;
      spans += conservative_span(out, row, &range);
   }
   counts[id] = spans;
}

KERNEL(1)
libkk_conservative_output_setup(global struct poly_geometry_params *geometry,
                                global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint n = p->clip_grid[0];
   uint count = n ? ((global uint *)p->clip_counts)[n - 1] : 0;
   p->output = (uintptr_t)poly_heap_alloc(heap, count * sizeof(struct kk_conservative_span), 8);
   p->draw[0] = count * 6;
}

KERNEL(64)
libkk_conservative_output(global struct poly_geometry_params *geometry)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   if (id >= p->clip_grid[0]) return;
   global uint *counts = (global uint *)p->clip_counts;
   uint first = id ? counts[id - 1] : 0;
   if (counts[id] == first)
      return;
   global struct kk_conservative_primitive *packet =
      (global struct kk_conservative_primitive *)((global uint64_t *)p->clipped)[id];
   global struct kk_conservative_span *spans = (global struct kk_conservative_span *)p->output;
   for (uint row = (uint)packet->bounds[1]; row < (uint)packet->bounds[3]; ++row) {
      float2 range;
      if (!conservative_span(packet, row, &range))
         continue;
      spans[first].primitive = (uintptr_t)packet;
      spans[first].bounds[0] = range.x;
      spans[first].bounds[1] = row;
      spans[first].bounds[2] = range.y;
      spans[first].bounds[3] = row + 1;
      ++first;
   }
}
