/* Copyright 2026 MacLauncher contributors
 * SPDX-License-Identifier: MIT
 * ON intervals are generated before fragment processing for stippled shaders
 * whose early depth/stencil tests have observable effects. */
#pragma once

struct kk_line_dash {
   uint32_t edge;
   float first;
   float last;
   float phase;
   float4 rectangle;
};

static float2
kk_line_screen(global struct poly_geometry_capture *p, uint viewport,
                const struct kk_polygon_vertex *v)
{
   return ((float2)(v->position[0], v->position[1]) / v->position[3] + 1.0f) *
      (float2)(p->viewport[viewport][2], p->viewport[viewport][3]) * 0.5f +
      (float2)(p->viewport[viewport][0], p->viewport[viewport][1]);
}

/* A major-axis pixel cell contains at most one Bresenham fragment. Keep the
 * original line's diamond-exit decision at its first and last cells; cutting
 * internal ON intervals at integer cell boundaries preserves native coverage. */
static bool
kk_line_diamond_exit(float2 a, float2 b, bool x_major, int column)
{
   float2 d = b - a;
   float major_a = x_major ? a.x : a.y;
   float major_d = x_major ? d.x : d.y;
   if (major_d == 0) return false;
   float minor = (x_major ? a.y : a.x) +
      ((float)column + 0.5f - major_a) * (x_major ? d.y : d.x) / major_d;
   float minor_cell = floor(minor);
   /* Apply Vulkan's infinitesimal (-epsilon, -epsilon^2) endpoint
    * perturbation symbolically at exact integer/diamond boundaries. */
   if (minor == minor_cell && (!x_major || d.y / d.x <= 0)) minor_cell--;
   float2 center = x_major ? (float2)((float)column + 0.5f, minor_cell + 0.5f) :
                              (float2)(minor_cell + 0.5f, (float)column + 0.5f);
   float2 x = a - center, y = b - center;
   float enter = 0, leave = 1;
   bool end_outside = false;
   for (uint plane = 0; plane < 4; ++plane) {
      float2 n = (float2)((plane & 2) ? -1.0f : 1.0f,
                          (plane & 1) ? -1.0f : 1.0f);
      float da = 0.5f - (x.x * n.x + x.y * n.y);
      float db = 0.5f - (y.x * n.x + y.y * n.y);
      if (da == 0) da = n.x * 1.0e-30f;
      if (db == 0) db = n.x * 1.0e-30f;
      end_outside |= db < 0;
      if (da < 0 && db < 0) return false;
      if ((da < 0) != (db < 0)) {
         float t = da / (da - db);
         if (da < 0) enter = fmax(enter, t);
         else leave = fmin(leave, t);
      }
   }
   return leave > enter && leave > 0 && (leave < 1 || end_outside);
}

static int4
kk_line_bres_bounds(float2 a, float2 b)
{
   if (!all(isfinite(a)) || !all(isfinite(b))) return (int4)(0, 0, 1, 1);
   float2 d = b - a;
   bool x_major = fabs(d.x) >= fabs(d.y);
   float first_value = x_major ? a.x : a.y;
   float last_value = x_major ? b.x : b.y;
   float difference = last_value - first_value;
   int direction = difference >= 0 ? 1 : -1;
   if (difference == 0) return (int4)(0, 0, direction, x_major);
   int first = (int)floor(first_value);
   int last = (int)floor(last_value);
   if (direction < 0 && first_value == (float)first) first--;
   if (direction > 0 && last_value == (float)last) last--;
   if (direction * (last - first) < 0) return (int4)(first, 0, direction, x_major);
   if (!kk_line_diamond_exit(a, b, x_major, first)) first += direction;
   if (direction * (last - first) < 0) return (int4)(first, 0, direction, x_major);
   if (!kk_line_diamond_exit(a, b, x_major, last)) last -= direction;
   return (int4)(first, max(direction * (last - first) + 1, 0), direction, x_major);
}

/* Vulkan wide Bresenham replicates the lowest minor-offset line. Its first
 * diamond and step count, rather than the unshifted centerline, define the
 * single shared stipple counter for every replica. */
static int4
kk_line_wide_bres_bounds(float2 a, float2 b, float width)
{
   float2 d = b - a;
   bool x_major = fabs(d.x) >= fabs(d.y);
   uint copies = max((uint)(width + 0.5f), 1u);
   float offset = -(float)(copies - 1u) * 0.5f;
   float2 translation = x_major ? (float2)(0, offset) : (float2)(offset, 0);
   return kk_line_bres_bounds(a + translation, b + translation);
}

/* Preserve exact integer lengths on the cardinal axes before ceil or phase. */
static float
kk_line_length(float2 delta)
{
   if (delta.y == 0) return fabs(delta.x);
   if (delta.x == 0) return fabs(delta.y);
   return sqrt(delta.x * delta.x + delta.y * delta.y);
}

static uint
kk_line_steps(float2 a, float2 b, uint mode, float width)
{
   if (mode == 2) return (uint)kk_line_wide_bres_bounds(a, b, width).y;
   float2 d = b - a;
   float length = kk_line_length(d);
   return isfinite(length) ? (uint)ceil(length) : 0;
}

static struct kk_line_dash
kk_line_make_dash(uint edge, float first, float last, float length,
                   float2 a, float2 b, uint mode, int4 bounds, uint phase)
{
   if (mode == 2) {
      float origin = bounds.w ? a.x : a.y;
      float delta = bounds.w ? b.x - a.x : b.y - a.y;
      float first_cell = bounds.z > 0 ? (float)bounds.x : (float)bounds.x + 1.0f;
      first = first <= 0 ? 0 : (first_cell + bounds.z * first - origin) / delta;
      last = last >= length ? 1 : (first_cell + bounds.z * last - origin) / delta;
   } else {
      first /= length;
      last /= length;
   }
   return (struct kk_line_dash){edge, clamp(first, 0.0f, 1.0f),
      clamp(last, 0.0f, 1.0f), (float)phase};
}

/* Two linear walks: count first, then write exactly that many merged ON runs.
 * No per-output search, retained worst-case varying packets or new raster
 * shader variants are needed. Phase is reduced only after uint64 prefix sums. */
static uint
kk_line_dashes(float2 a, float2 b, uint mode, uint pattern, uint factor,
                 uint phase, uint edge, global struct kk_line_dash *spans)
{
   int4 bounds = mode == 2 ? kk_line_bres_bounds(a, b) : (int4)(0);
   float2 d = b - a;
   float length = mode == 2 ? (float)bounds.y : kk_line_length(d);
   if (!(length > 0) || !isfinite(length) || !(pattern & 0xffffu)) return 0;
   factor = max(factor, 1u);
   phase %= factor * 16u;
   if ((pattern & 0xffffu) == 0xffffu) {
      if (spans) spans[0] = (struct kk_line_dash){edge, 0, 1, (float)phase};
      return 1;
   }
   float cursor = 0, run = -1;
   uint count = 0;
   while (cursor < length) {
      uint counter = phase + (uint)cursor;
      bool on = pattern & (1u << ((counter / factor) & 15u));
      if (on && run < 0) run = cursor;
      if (!on && run >= 0) {
         if (spans) spans[count] = kk_line_make_dash(edge, run, cursor, length,
            a, b, mode, bounds, phase);
         count++;
         run = -1;
      }
      cursor = fmin(length, cursor + (float)(factor - counter % factor));
   }
   if (run >= 0) {
      if (spans) spans[count] = kk_line_make_dash(edge, run, length, length,
         a, b, mode, bounds, phase);
      count++;
   }
   return count;
}

static uint64_t
kk_line_primitive_phase(global struct poly_geometry_params *geometry,
                         global struct poly_geometry_capture *p, uint id,
                         uint invocation, uint stream, uint4 vertices)
{
   if (!p->line_phases) return 0;
   uint maximum = sub_sat(p->vertices_out, 1u);
   uint first = (invocation * p->streams + stream) * maximum + (vertices.w & 0xffffu);
   if (p->line_strip) {
      uint per_instance = max(geometry->grid[0], 1u);
      uint instance_first = invocation - invocation % per_instance;
      uint strip_first = p->restart_strips ?
         ((global uint *)p->restart_strips)[invocation % per_instance] : 0;
      first = (instance_first + strip_first) * p->streams * maximum;
   }
   global uint64_t *phases = (global uint64_t *)p->line_phases;
   return (id ? phases[id - 1] : 0) - (first ? phases[first - 1] : 0);
}

/* Only the observable-early path reserves this sidecar. Ordinary polygons
 * keep their original allocation size and per-draw parameter layout. */
static global uint64_t *
kk_line_dash_pointer(global struct kk_polygon_vertex *polygon, uint count)
{
   uint offset = align(count * sizeof(struct kk_polygon_vertex), 8);
   return (global uint64_t *)((global uchar *)polygon + offset);
}

/* Only the geometry-stipple capture has this variable-sized tail. Locations
 * come from the same Vulkan state consumed by the native render pass. */
static uint
kk_line_sample_count(global struct poly_geometry_capture *p)
{
   return *((global uint *)(p + 1));
}

static float2
kk_line_sample_location(global struct poly_geometry_capture *p, uint sample)
{
   global float2 *locations = (global float2 *)((global uint *)(p + 1) + 2);
   return locations[sample];
}

/* Vulkan advertises eight raster subpixel bits. Evaluate the original
 * rectangle on that fixed lattice before making sample-cell stair steps;
 * subdividing the original edge must not create a different outer edge. */
static int64_t
kk_line_edge(int2 a, int2 b, int2 point)
{
   return (int64_t)(b.x - a.x) * (point.y - a.y) -
          (int64_t)(b.y - a.y) * (point.x - a.x);
}

static bool
kk_line_original_coverage(private int2 *corners, int2 point)
{
   bool reverse = kk_line_edge(corners[0], corners[1], corners[2]) < 0;
   for (uint i = 0; i < 4; ++i) {
      int2 a = corners[reverse ? (i + 1) % 4 : i];
      int2 b = corners[reverse ? i : (i + 1) % 4];
      int64_t edge = kk_line_edge(a, b, point);
      int2 delta = b - a;
      bool top_left = delta.y < 0 || (delta.y == 0 && delta.x > 0);
      if (edge < 0 || (edge == 0 && !top_left)) return false;
   }
   return true;
}

/* One run covers consecutive sample cells in a single unique sample-Y row.
 * Midpoints, including the neighboring pixel's sample, are exact multiples
 * of 1/32 for Vulkan's four-bit sample locations, well inside the raster
 * lattice. Coincident samples share a cell and the same keep decision. */
static uint
kk_line_sample_rectangles(global struct poly_geometry_capture *p,
                            float2 a, float2 b, uint pattern, uint factor,
                            uint phase, uint edge,
                            global struct kk_line_dash *spans)
{
   if (!(pattern & 0xffffu)) return 0;
   float2 delta = b - a;
   float length = kk_line_length(delta);
   if (!(length > 0) || !isfinite(length)) return 0;
   float2 direction = delta / length;
   /* Exact cardinal tangents keep integer stipple boundaries exact even
    * when Metal fast division approximates reciprocal length. */
   if (delta.y == 0) direction = (float2)(delta.x < 0 ? -1.0f : 1.0f, 0);
   if (delta.x == 0) direction = (float2)(0, delta.y < 0 ? -1.0f : 1.0f);
   float cap = p->line_mode == 3 ? 0.5f : 0;
   float half_width = (p->line_width + (p->line_mode == 3 ? 1.0f : 0)) * 0.5f;
   float2 normal = (float2)(-direction.y, direction.x) * half_width;
   float2 start = a - direction * cap, end = b + direction * cap;
   float2 corners[4] = {start - normal, start + normal, end + normal, end - normal};
   int2 fixed[4];
   float2 minimum = corners[0], maximum = corners[0];
   for (uint i = 0; i < 4; ++i) {
      fixed[i] = convert_int2_rte(corners[i] * 256.0f);
      minimum = fmin(minimum, corners[i]); maximum = fmax(maximum, corners[i]);
   }
   int first_x = max((int)floor(minimum.x) - 1, 0);
   int last_x = min((int)ceil(maximum.x) + 1, (int)p->render_size[0]);
   int first_y = max((int)floor(minimum.y) - 1, 0);
   int last_y = min((int)ceil(maximum.y) + 1, (int)p->render_size[1]);
   uint samples = kk_line_sample_count(p);
   factor = max(factor, 1u);
   uint count = 0;
   for (uint row_sample = 0; row_sample < samples; ++row_sample) {
      float y = kk_line_sample_location(p, row_sample).y;
      bool repeated_y = false;
      float previous_y = y - 1.0f, next_y = y + 1.0f;
      for (uint s = 0; s < samples; ++s) {
         float candidate = kk_line_sample_location(p, s).y;
         repeated_y |= s < row_sample && candidate == y;
         if (candidate < y) previous_y = fmax(previous_y, candidate);
         if (candidate > y) next_y = fmin(next_y, candidate);
      }
      if (repeated_y) continue;
      if (previous_y == y - 1.0f)
         for (uint s = 0; s < samples; ++s)
            previous_y = fmax(previous_y, kk_line_sample_location(p, s).y - 1.0f);
      if (next_y == y + 1.0f)
         for (uint s = 0; s < samples; ++s)
            next_y = fmin(next_y, kk_line_sample_location(p, s).y + 1.0f);
      /* Sort unique X events for this Y without a fixed per-draw array. */
      for (int pixel_y = first_y; pixel_y < last_y; ++pixel_y) {
         float lower_y = pixel_y + (previous_y + y) * 0.5f;
         float upper_y = pixel_y + (y + next_y) * 0.5f;
         bool running = false;
         float run_start = 0, run_end = 0;
         for (int pixel_x = first_x; pixel_x < last_x; ++pixel_x) {
            float after = -1.0f;
            for (uint event = 0; event < samples; ++event) {
               float x = 2.0f;
               for (uint s = 0; s < samples; ++s) {
                  float2 location = kk_line_sample_location(p, s);
                  if (location.y == y && location.x > after) x = fmin(x, location.x);
               }
               if (x > 1.0f) break;
               float previous_x = x - 1.0f, next_x = x + 1.0f;
               for (uint s = 0; s < samples; ++s) {
                  float2 location = kk_line_sample_location(p, s);
                  if (location.y != y) continue;
                  if (location.x < x) previous_x = fmax(previous_x, location.x);
                  if (location.x > x) next_x = fmin(next_x, location.x);
               }
               if (previous_x == x - 1.0f)
                  for (uint s = 0; s < samples; ++s) {
                     float2 location = kk_line_sample_location(p, s);
                     if (location.y == y) previous_x = fmax(previous_x, location.x - 1.0f);
                  }
               if (next_x == x + 1.0f)
                  for (uint s = 0; s < samples; ++s) {
                     float2 location = kk_line_sample_location(p, s);
                     if (location.y == y) next_x = fmin(next_x, location.x + 1.0f);
                  }
               float2 point = (float2)(pixel_x + x, pixel_y + y);
               float along = (point.x - a.x) * direction.x + (point.y - a.y) * direction.y;
               uint counter = phase + (uint)fmax(floor(along), 0.0f);
               bool on = kk_line_original_coverage(fixed, convert_int2_rte(point * 256.0f)) &&
                  (pattern & (1u << ((counter / factor) & 15u)));
               float lower_x = pixel_x + (previous_x + x) * 0.5f;
               float upper_x = pixel_x + (x + next_x) * 0.5f;
               if (on) {
                  if (!running) run_start = lower_x;
                  run_end = upper_x; running = true;
               } else if (running) {
                  if (spans) spans[count] = (struct kk_line_dash){
                     .edge = edge, .phase = (float)phase,
                     .rectangle = (float4)(run_start, lower_y, run_end, upper_y)};
                  count++; running = false;
               }
               after = x;
            }
         }
         if (running) {
            if (spans) spans[count] = (struct kk_line_dash){
               .edge = edge, .phase = (float)phase,
               .rectangle = (float4)(run_start, lower_y, run_end, upper_y)};
            count++;
         }
      }
   }
   return count;
}

static uint
kk_line_geometry_dashes(global struct poly_geometry_capture *p, float2 a, float2 b,
                          uint pattern, uint factor, uint phase, uint edge,
                          global struct kk_line_dash *spans)
{
   if (p->line_mode != 2)
      return kk_line_sample_rectangles(p, a, b, pattern, factor, phase, edge, spans);
   uint copies = (uint)(p->line_width + 0.5f), total = 0;
   int4 original = kk_line_wide_bres_bounds(a, b, p->line_width);
   uint period = max(factor, 1u) * 16u;
   for (uint copy = 0; copy < copies; ++copy) {
      float offset = (float)copy - (float)(copies - 1u) * 0.5f;
      float2 translation = original.w ? (float2)(0, offset) : (float2)(offset, 0);
      int4 replica = kk_line_bres_bounds(a + translation, b + translation);
      int origin_delta = original.z * (replica.x - original.x);
      uint replica_phase = (uint)(((int)phase + origin_delta + (int)period) % (int)period);
      uint count = kk_line_dashes(a + translation, b + translation, 2, pattern,
         factor, replica_phase, edge, spans ? spans + total : NULL);
      if (spans) for (uint i = 0; i < count; ++i) {
         spans[total + i].rectangle = (float4)(offset, 0, 0, 0);
         spans[total + i].phase = (float)phase;
      }
      total += count;
   }
   return total;
}
