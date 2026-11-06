/* SPDX-License-Identifier: MIT
 * Native point/line assembly from the existing once-only Poly capture.
 * Included by kk_polygon.cl to share its capture and clipping helpers. */

#include "../vulkan/kk_interlock_envelope.h"
#include "kk_interlock_source_distance.h"

KERNEL(1)
libkk_interlock_source_capture_setup(global struct poly_geometry_params *geometry,
                                    global struct poly_heap *heap, uint arity)
{
   polygon_capture_setup(geometry, heap, arity, true);
   global struct kk_interlock_envelope *envelope =
      (global struct kk_interlock_envelope *)polygon_capture(geometry);
   envelope->source_dispatch[0] = (envelope->geometry.clip_grid[0] + 63u) / 64u;
   envelope->source_dispatch[1] = 1;
   envelope->source_dispatch[2] = 1;
}

static bool
interlock_source_primitive(global struct poly_geometry_capture *p, uint id,
   uint arity, uint *invocation, uint *stream, uint4 *vertices)
{
   uint maximum = sub_sat(p->vertices_out, arity - 1);
   if (!maximum)
      return false;
   uint stream_index = id / maximum;
   uint primitive = id % maximum;
   *stream = stream_index % p->streams;
   *invocation = stream_index / p->streams;
   if (*invocation >= p->invocations || *stream != p->rasterization_stream ||
       primitive >= ((global uint *)p->primitive_counts)[stream_index])
      return false;
   *vertices = ((global uint4 *)p->triangles)[id];
   return true;
}

static float
interlock_source_plane_distance(global struct poly_geometry_capture *p,
   global uint4 *input, float4 position, uint plane)
{
   float distance = 0;
   switch (plane) {
   case 0: distance = position.w + position.x; break;
   case 1: distance = position.w - position.x; break;
   case 2: distance = position.w + position.y; break;
   case 3: distance = position.w - position.y; break;
   case 4: distance = position.z + (p->negative_one_to_one ? position.w : 0); break;
   case 5: distance = position.w - position.z; break;
   default: {
      uint c = plane - 6;
      float4 distances = as_float4(input[polygon_slot(p,
         VARYING_SLOT_CLIP_DIST0 + c / 4)]);
      distance = distances[c % 4];
      break;
   }
   }
   return distance;
}

/* Native point and line clipping has fixed output arity for every survivor.
 * Setup reserves its records together with the pointer table, so this pass
 * needs no per-primitive allocator or mutable private endpoint arrays. */
KERNEL(64)
libkk_interlock_source_clip(global struct poly_geometry_params *geometry,
   global struct poly_heap *heap, uint arity)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   if (id >= p->clip_grid[0])
      return;
   global uint *counts = (global uint *)p->clip_counts;
   global uint64_t *clipped = (global uint64_t *)p->clipped;
   counts[id] = 0;
   clipped[id] = 0;
   uint invocation = 0, stream = 0;
   uint4 vertices = 0;
   bool valid = interlock_source_primitive(p, id, arity,
      &invocation, &stream, &vertices);
   float4 a = 0, b = 0;
   float first = 0, last = 1;
   if (valid) {
      global uint4 *input_a = polygon_vertex(p, invocation, stream, vertices.x);
      global uint4 *input_b = arity == 1 ? input_a :
         polygon_vertex(p, invocation, stream, vertices.y);
      uint position_slot = polygon_slot(p, VARYING_SLOT_POS);
      a = as_float4(input_a[position_slot]);
      b = as_float4(input_b[position_slot]);
      for (uint c = 0; c < p->cull_count; ++c) {
         float da = interlock_source_cull_distance(p, input_a, c);
         float db = interlock_source_cull_distance(p, input_b, c);
         if (da < 0 && db < 0)
            valid = false;
      }
      for (uint plane = 0; plane < 6 + p->clip_count; ++plane) {
         if (p->depth_clip || (plane != 4 && plane != 5)) {
            float da = interlock_source_plane_distance(p, input_a, a, plane);
            float db = interlock_source_plane_distance(p, input_b, b, plane);
            if (da < 0 && db < 0)
               valid = false;
            if (arity == 2 && ((da < 0) != (db < 0))) {
               float intersection = da / (da - db);
               if (da < 0) first = fmax(first, intersection);
               else last = fmin(last, intersection);
            }
         }
      }
      if (first > last)
         valid = false;
   }
   if (!valid)
      return;
   uint64_t record_base = (uint64_t)p->clipped +
      align(p->clip_grid[0] * 8u, 16u);
   global struct kk_polygon_vertex *output =
      (global struct kk_polygon_vertex *)(record_base +
         (uint64_t)id * arity * sizeof(struct kk_polygon_vertex));
   float4 start = arity == 1 ? a : a + first * (b - a);
   output[0].position[0] = start.x;
   output[0].position[1] = start.y;
   output[0].position[2] = start.z;
   output[0].position[3] = start.w;
   output[0].weight[0] = arity == 1 ? 1 : 1 - first;
   output[0].weight[1] = arity == 1 ? 0 : first;
   output[0].weight[2] = 0;
   if (arity == 2) {
      float4 end = a + last * (b - a);
      output[1].position[0] = end.x;
      output[1].position[1] = end.y;
      output[1].position[2] = end.z;
      output[1].position[3] = end.w;
      output[1].weight[0] = 1 - last;
      output[1].weight[1] = last;
      output[1].weight[2] = 0;
   }
   clipped[id] = (uintptr_t)output;
   counts[id] = arity;
}

KERNEL(1)
libkk_interlock_source_output_setup(global struct poly_geometry_params *geometry,
   global struct poly_heap *heap)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint entries = p->clip_grid[0];
   uint count = entries ? ((global uint *)p->clip_counts)[entries - 1] : 0;
   p->output = (uintptr_t)poly_heap_alloc(heap, count * (p->vertex_stride + 16), 16);
   p->draw[0] = count;
}

KERNEL(64)
libkk_interlock_source_output(global struct poly_geometry_params *geometry, uint arity)
{
   global struct poly_geometry_capture *p = polygon_capture(geometry);
   uint id = cl_global_id.x;
   if (id >= p->clip_grid[0])
      return;
   global uint *counts = (global uint *)p->clip_counts;
   uint first = id ? counts[id - 1] : 0;
   uint count = counts[id] - first;
   if (!count)
      return;
   uint invocation, stream;
   uint4 vertices;
   if (!interlock_source_primitive(p, id, arity, &invocation, &stream, &vertices))
      return;
   global struct kk_polygon_vertex *clipped =
      (global struct kk_polygon_vertex *)((global uint64_t *)p->clipped)[id];
   uint provoking = p->provoking_last ? arity - 1 : 0;
   global uint4 *provoking_output = polygon_vertex(p, invocation, stream, vertices[provoking]);
   uint viewport = p->outputs & (1ul << VARYING_SLOT_VIEWPORT) ?
      provoking_output[polygon_slot(p, VARYING_SLOT_VIEWPORT)].x : 0;
   float positions[3][4] = {{0}};
   for (uint i = 0; i < arity; ++i) {
      float4 position = polygon_position(p, invocation, stream, vertices[i]);
      for (uint c = 0; c < 4; ++c)
         positions[i][c] = position[c];
   }
   float bias = polygon_depth_bias(p, viewport, clipped, arity);
   for (uint i = 0; i < arity; ++i) {
      struct kk_polygon_vertex vertex = clipped[i];
      global uint4 *output = (global uint4 *)(p->output +
         (uint64_t)(first + i) * (p->vertex_stride + 16));
      uint packed = 0;
      for (uint slot = 0; slot < 64; ++slot) {
         if (!(p->outputs & (1ul << slot)))
            continue;
         uint4 value = provoking_output[packed];
         if ((p->float_outputs & (1ul << slot)) &&
             !(geometry->flat_outputs & (1ul << slot))) {
            float4 interpolated;
            for (uint c = 0; c < 4; ++c) {
               float input[3] = {0};
               for (uint v = 0; v < arity; ++v)
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
               z = minimum == maximum ? 0 : (z - minimum) / (maximum - minimum);
            }
            value = as_uint4((float4)(vertex.position[0], vertex.position[1],
               z * vertex.position[3], vertex.position[3]));
         }
         output[packed++] = value;
      }
      output[packed] = (uint4)(1, 0, 0, 0);
   }
}
