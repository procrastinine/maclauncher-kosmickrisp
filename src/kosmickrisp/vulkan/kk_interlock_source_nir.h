/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SOURCE_NIR_H
#define KK_INTERLOCK_SOURCE_NIR_H

#include "compiler/nir/nir_builder.h"
#include "poly/geometry.h"
#include "poly/geometry_capture.h"

static nir_def *
kk_interlock_source_parameters(nir_builder *b)
{
   return nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, nir_load_geometry_param_buffer_poly(b),
         offsetof(struct poly_geometry_params, capture)), .align_mul = 8);
}

static nir_def *
kk_interlock_source_load(nir_builder *b, unsigned components, unsigned bits,
   unsigned offset, unsigned alignment)
{
   return nir_load_global_constant(b, components, bits,
      nir_iadd_imm(b, kk_interlock_source_parameters(b), offset),
      .align_mul = alignment);
}

static nir_def *
kk_interlock_source_point_size(nir_builder *b, nir_def *size)
{
   size = nir_fmin(b, nir_fmax(b, size, nir_imm_float(b, 1)), nir_imm_float(b, 511));
   return nir_fmul_imm(b, nir_fround_even(b, nir_fmul_imm(b, size, 16)), 1.0f / 16.0f);
}

/* Original FS lowering reads one float predicate for every cull distance and
 * discards only when its interpolated predicate is zero. The assembly kernel
 * has already rejected whole primitives, so every surviving raster vertex
 * supplies one. This retains the shared FS interface without applying a
 * second per-fragment approximation to the completed primitive cull. */
static void
kk_interlock_source_resolved_cull(nir_shader *shader,
   const struct poly_gs_capture_info *info)
{
   if (!info->cull_count)
      return;
   nir_builder b = nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(shader)));
   for (unsigned i = 0; i < info->cull_count; ++i)
      nir_store_output(&b, nir_imm_float(&b, 1), nir_imm_int(&b, 0),
         .src_type = nir_type_float32, .component = i & 3, .write_mask = 1,
         .io_semantics = {.location = VARYING_SLOT_CULL_PRIMITIVE + i / 4,
            .num_slots = 1});
   nir_shader_gather_info(shader, nir_shader_get_entrypoint(shader));
}

/* The compact output uses the same record layout as clipped triangles. Native
 * points retain PointSize and native lines retain two endpoints; no source
 * vertex or geometry shader is invoked by this rasterization shader. */
static nir_shader *
kk_interlock_source_vertex_shader(const struct poly_gs_capture_info *info,
   const nir_shader_compiler_options *options, unsigned arity)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      options, "interlock captured source rasterization");
   b.shader->info.io_lowered = true;
   unsigned slots = util_bitcount64(info->outputs);
   nir_def *address = nir_iadd(&b,
      kk_interlock_source_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
      nir_u2u64(&b, nir_imul_imm(&b, nir_load_vertex_id(&b), (slots + 1) * 16)));
   nir_def *viewport = nir_imm_int(&b, 0);
   if (info->outputs & VARYING_BIT_VIEWPORT) {
      unsigned slot = util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_VIEWPORT));
      viewport = nir_load_global_constant(&b, 1, 32,
         nir_iadd_imm(&b, address, slot * 16), .align_mul = 16);
   }
   nir_def *viewport_address = nir_iadd(&b, kk_interlock_source_parameters(&b),
      nir_u2u64(&b, nir_iadd_imm(&b, nir_imul_imm(&b, viewport, 6 * 4),
         offsetof(struct poly_geometry_capture, viewport))));
   nir_def *vp = nir_load_global_constant(&b, 4, 32, viewport_address, .align_mul = 4);
   nir_def *size = kk_interlock_source_load(&b, 2, 32,
      offsetof(struct poly_geometry_capture, render_size), 4);
   unsigned packed = 0;
   u_foreach_bit64(slot, info->outputs) {
      nir_def *value = nir_load_global_constant(&b, 4, 32,
         nir_iadd_imm(&b, address, packed++ * 16), .align_mul = 16);
      if (slot == VARYING_SLOT_PSIZ && arity != 1)
         continue;
      if (slot == VARYING_SLOT_POS) {
         nir_def *w = nir_channel(&b, value, 3);
         nir_def *screen = nir_ffma(&b,
            nir_fmul_imm(&b, nir_fadd_imm(&b,
               nir_fdiv(&b, nir_channels(&b, value, 3), w), 1), 0.5f),
            nir_channels(&b, vp, 12), nir_channels(&b, vp, 3));
         nir_def *position = nir_fmul(&b,
            nir_fadd_imm(&b, nir_fmul_imm(&b, nir_fdiv(&b, screen, size), 2), -1), w);
         value = nir_vec4(&b, nir_channel(&b, position, 0),
            nir_channel(&b, position, 1), nir_channel(&b, value, 2), w);
      } else {
         value = nir_convert_to_bit_size(&b, value,
            nir_alu_type_get_base_type(info->output_types[slot]),
            nir_alu_type_get_type_size(info->output_types[slot]));
      }
      if (slot == VARYING_SLOT_PSIZ)
         value = kk_interlock_source_point_size(&b, nir_channel(&b, value, 0));
      nir_store_output(&b, nir_trim_vector(&b, value, info->output_components[slot]),
         nir_imm_int(&b, 0), .src_type = info->output_types[slot],
         .io_semantics = {.location = slot, .num_slots = 1});
   }
   if (arity == 1 && !(info->outputs & VARYING_BIT_PSIZ))
      nir_store_output(&b, nir_imm_float(&b, 1), nir_imm_int(&b, 0),
         .src_type = nir_type_float32,
         .io_semantics = {.location = VARYING_SLOT_PSIZ, .num_slots = 1});
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

/* Vulkan permits PointCoord at the pixel center even with sample shading.
 * Evaluate it from the exact captured point and its clamped native size; the
 * original helper lanes therefore have defined extrapolated coordinates. */
static nir_def *
kk_interlock_source_point_coord(nir_builder *b,
   const struct poly_gs_capture_info *info, nir_def *params,
   nir_def *primitive, nir_def *pixel_center)
{
   if (!info || !(info->outputs & VARYING_BIT_POS))
      return nir_imm_vec2(b, 0.5f, 0.5f);
   unsigned slots = util_bitcount64(info->outputs);
   nir_def *output = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, params, offsetof(struct poly_geometry_capture, output)),
      .align_mul = 8);
   nir_def *address = nir_iadd(b, output,
      nir_u2u64(b, nir_imul_imm(b, primitive, (slots + 1) * 16)));
   unsigned position_slot = util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_POS));
   nir_def *position = nir_load_global_constant(b, 4, 32,
      nir_iadd_imm(b, address, position_slot * 16), .align_mul = 16);
   nir_def *size = nir_imm_float(b, 1);
   if (info->outputs & VARYING_BIT_PSIZ) {
      unsigned slot = util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_PSIZ));
      size = nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, address, slot * 16), .align_mul = 16);
      size = kk_interlock_source_point_size(b, size);
   }
   nir_def *viewport = nir_imm_int(b, 0);
   if (info->outputs & VARYING_BIT_VIEWPORT) {
      unsigned slot = util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_VIEWPORT));
      viewport = nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, address, slot * 16), .align_mul = 16);
   }
   nir_def *vp = nir_load_global_constant(b, 4, 32,
      nir_iadd(b, nir_iadd_imm(b, params, offsetof(struct poly_geometry_capture, viewport)),
         nir_u2u64(b, nir_imul_imm(b, viewport, 6 * 4))), .align_mul = 4);
   nir_def *center = nir_ffma(b,
      nir_fmul_imm(b, nir_fadd_imm(b,
         nir_fdiv(b, nir_channels(b, position, 3), nir_channel(b, position, 3)), 1), 0.5f),
      nir_channels(b, vp, 12), nir_channels(b, vp, 3));
   return nir_fadd_imm(b, nir_fdiv(b, nir_fsub(b, pixel_center, center), size), 0.5f);
}

#endif
