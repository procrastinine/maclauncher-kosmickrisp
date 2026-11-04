/* SPDX-License-Identifier: MIT */
/* Quantization can make consecutive clipped vertices locally nonconvex.
 * Gift wrapping constructs the hull before applying the inner half-grid
 * margin. This is bounded by KK_POLYGON_MAX_VERTICES and runs only for an
 * enabled conservative draw whose fragment shader reads FullyCovered. */
static long
kk_inner_cross(long2 a, long2 b)
{
   return a.x * b.y - a.y * b.x;
}

static uintptr_t
kk_conservative_build_inner(global struct poly_heap *heap,
   global struct kk_polygon_vertex *polygon, uint count,
   global const float *vp, bool minus_one)
{
   long2 points[KK_POLYGON_MAX_VERTICES];
   uint start = 0;
   for (uint i = 0; i < count; ++i) {
      float2 xy = conservative_window(vload4(0, polygon[i].position), vp,
                                       minus_one).xy;
      points[i] = convert_long2(rint(xy * 256.0f));
      if (i && (points[i].x < points[start].x ||
                (points[i].x == points[start].x && points[i].y < points[start].y)))
         start = i;
   }
   global struct kk_conservative_inner_edge *edges =
      poly_heap_alloc(heap, count * sizeof(*edges), 8);
   uint current = start, written = 0;
   do {
      uint next = current;
      for (uint i = 0; i < count; ++i) {
         long2 a = points[next] - points[current];
         long2 b = points[i] - points[current];
         long cross = kk_inner_cross(a, b);
         long da = a.x * a.x + a.y * a.y;
         long db = b.x * b.x + b.y * b.y;
         if (next == current || cross < 0 || (cross == 0 && db > da))
            next = i;
      }
      long2 a = points[current], b = points[next];
      long A = a.y - b.y, B = b.x - a.x;
      edges[written].a = A;
      edges[written].b = B;
      edges[written].c = -(A * a.x + B * a.y);
      ++written;
      current = next;
   } while (current != start && written < count);
   /* Reuse the original packet's count. A zero edge imposes no constraint. */
   for (uint i = written; i < count; ++i) {
      edges[i].a = 0;
      edges[i].b = 0;
      edges[i].c = 0;
   }
   return (uintptr_t)edges;
}
