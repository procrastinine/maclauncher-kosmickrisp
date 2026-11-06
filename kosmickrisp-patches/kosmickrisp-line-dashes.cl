
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
   counts[id] = written * 2u;
}
