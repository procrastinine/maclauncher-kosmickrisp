/*
 * Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT
 *
 * Homogeneous polygon clipping follows Mesa's draw_pipe_clip.c (Mesa commit
 * 4f071f891d63396c799b267172c19ba6b0301682). Carry original-triangle weights
 * through clipping instead of copying every varying into temporary arrays.
 */
#ifndef KK_POLYGON_H
#define KK_POLYGON_H

#ifndef __OPENCL_VERSION__
#include <stdbool.h>
#include <stdint.h>
#endif

#define KK_POLYGON_CLIP_DISTANCES 8
#define KK_POLYGON_MAX_VERTICES (3 + 6 + KK_POLYGON_CLIP_DISTANCES)

struct kk_polygon_vertex {
   float position[4];
   float weight[3];
};

static inline float
kk_polygon_plane_distance(const struct kk_polygon_vertex *v, unsigned plane,
                          const float clip[3][KK_POLYGON_CLIP_DISTANCES],
                          bool negative_one_to_one)
{
   switch (plane) {
   case 0: return v->position[3] + v->position[0];
   case 1: return v->position[3] - v->position[0];
   case 2: return v->position[3] + v->position[1];
   case 3: return v->position[3] - v->position[1];
   case 4:
      return v->position[2] +
             (negative_one_to_one ? v->position[3] : 0.0f);
   case 5: return v->position[3] - v->position[2];
   default:
      return v->weight[0] * clip[0][plane - 6] +
             v->weight[1] * clip[1][plane - 6] +
             v->weight[2] * clip[2][plane - 6];
   }
}

static inline struct kk_polygon_vertex
kk_polygon_intersection(const struct kk_polygon_vertex *a,
                        const struct kk_polygon_vertex *b, float t)
{
   struct kk_polygon_vertex out;
   for (unsigned i = 0; i < 4; ++i)
      out.position[i] = a->position[i] + t * (b->position[i] - a->position[i]);
   for (unsigned i = 0; i < 3; ++i)
      out.weight[i] = a->weight[i] + t * (b->weight[i] - a->weight[i]);
   return out;
}

/* A clipped triangle gains at most one vertex per plane. The Vulkan clip/cull
 * distance limit bounds this storage; it is not a draw-size or resource quota.
 * Cull distances act on the original primitive before clipping. */
static inline unsigned
kk_polygon_clip(const float position[3][4],
                const float clip[3][KK_POLYGON_CLIP_DISTANCES],
                const float cull[3][KK_POLYGON_CLIP_DISTANCES],
                unsigned clip_count, unsigned cull_count,
                bool depth_clip, bool negative_one_to_one,
                struct kk_polygon_vertex out[KK_POLYGON_MAX_VERTICES])
{
   for (unsigned i = 0; i < cull_count; ++i) {
      if (cull[0][i] < 0.0f && cull[1][i] < 0.0f && cull[2][i] < 0.0f)
         return 0;
   }

   struct kk_polygon_vertex a[KK_POLYGON_MAX_VERTICES];
   struct kk_polygon_vertex b[KK_POLYGON_MAX_VERTICES];
   for (unsigned i = 0; i < 3; ++i) {
      for (unsigned c = 0; c < 4; ++c)
         a[i].position[c] = position[i][c];
      for (unsigned c = 0; c < 3; ++c)
         a[i].weight[c] = i == c ? 1.0f : 0.0f;
   }

   unsigned count = 3;
   for (unsigned plane = 0; plane < 6 + clip_count && count; ++plane) {
      if (!depth_clip && (plane == 4 || plane == 5))
         continue;

      unsigned written = 0;
      struct kk_polygon_vertex previous = a[count - 1];
      float previous_d = kk_polygon_plane_distance(
         &previous, plane, clip, negative_one_to_one);
      for (unsigned i = 0; i < count; ++i) {
         struct kk_polygon_vertex current = a[i];
         float current_d = kk_polygon_plane_distance(
            &current, plane, clip, negative_one_to_one);
         bool previous_inside = previous_d >= 0.0f;
         bool current_inside = current_d >= 0.0f;
         if (previous_inside != current_inside) {
            float t = previous_d / (previous_d - current_d);
            b[written++] = kk_polygon_intersection(&previous, &current, t);
         }
         if (current_inside)
            b[written++] = current;
         previous = current;
         previous_d = current_d;
      }
      count = written;
      /* Keep private storage explicit. A generic-address pointer phi between
       * private arrays otherwise forces unrelated OpenCL address spaces into
       * every load/store in the Metal kernel. */
      for (unsigned i = 0; i < count; ++i)
         a[i] = b[i];
   }
   for (unsigned i = 0; i < count; ++i)
      out[i] = a[i];
   return count;
}

/* Line clipping shares the polygon plane distances, original-vertex weights
 * and intersection interpolation. Cull distances reject an entire segment. */
static inline unsigned
kk_polygon_clip_line(const float position[3][4],
                     const float clip[3][KK_POLYGON_CLIP_DISTANCES],
                     const float cull[3][KK_POLYGON_CLIP_DISTANCES],
                     unsigned clip_count, unsigned cull_count,
                     bool depth_clip, bool negative_one_to_one,
                     struct kk_polygon_vertex out[KK_POLYGON_MAX_VERTICES])
{
   for (unsigned i = 0; i < cull_count; ++i)
      if (cull[0][i] < 0 && cull[1][i] < 0) return 0;
   for (unsigned v = 0; v < 2; ++v) {
      for (unsigned c = 0; c < 4; ++c) out[v].position[c] = position[v][c];
      for (unsigned c = 0; c < 3; ++c) out[v].weight[c] = v == c ? 1.0f : 0.0f;
   }
   for (unsigned plane = 0; plane < 6 + clip_count; ++plane) {
      if (!depth_clip && (plane == 4 || plane == 5)) continue;
      float a = kk_polygon_plane_distance(&out[0], plane, clip, negative_one_to_one);
      float b = kk_polygon_plane_distance(&out[1], plane, clip, negative_one_to_one);
      if (a < 0 && b < 0) return 0;
      if ((a < 0) != (b < 0)) {
         struct kk_polygon_vertex intersection =
            kk_polygon_intersection(&out[0], &out[1], a / (a - b));
         out[a < 0 ? 0 : 1] = intersection;
      }
   }
   return 2;
}

/* Perspective varyings interpolate with homogeneous clipping weights. Linear
 * screen-space varyings use the same weights multiplied by original W / new W,
 * as in Mesa's clipper. Integer and flat outputs keep the provoking value. */
static inline float
kk_polygon_varying(const struct kk_polygon_vertex *vertex,
                   const float position[3][4], const float varying[3],
                   bool noperspective)
{
   float value = 0.0f;
   for (unsigned i = 0; i < 3; ++i) {
      float weight = vertex->weight[i];
      if (noperspective)
         weight *= position[i][3] / vertex->position[3];
      value += weight * varying[i];
   }
   return value;
}

#endif
