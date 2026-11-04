/*
 * SPDX-License-Identifier: MIT
 */

#include "kk_nir_lower_per_vertex.h"

#include "kk_cmd_buffer.h"
#include "kk_nir_vertex_triplet.h"
#include "kk_debug.h"

#include "nir_builder.h"
#include "vk_util.h"

static bool
is_per_vertex_input(nir_intrinsic_instr *intr)
{
   return intr->intrinsic == nir_intrinsic_load_per_vertex_input ||
          intr->intrinsic == nir_intrinsic_load_input_vertex;
}

static unsigned
io_location(nir_intrinsic_instr *intr, unsigned offset_src)
{
   return nir_intrinsic_io_semantics(intr).location +
          nir_src_as_uint(intr->src[offset_src]);
}

/* Position occupies four native components. Raster system inputs share the
 * remaining 124 with ordinary varyings. The current MSL system inputs occupy
 * at most 32 components; use that bound to skip small interfaces entirely. */
static unsigned
kk_native_interface_components(nir_shader *fs, nir_shader *prev, uint8_t *out_width)
{
   const uint64_t generic = BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
      ~BITFIELD64_MASK(VARYING_SLOT_VAR0);
   uint64_t fixed_outputs = (prev ? prev->info.outputs_written : 0) &
      ~(generic | VARYING_BIT_POS);
   uint8_t width[VARYING_SLOT_MAX] = {0};
   bool sample_id = false, sample_mask = false, front_face = false;
   bool primitive_id = false, point_coord = false, layer = false;
   bool subgroup_size = false, subgroup_invocation = false;
   bool barycentric[2] = {false, false};
   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         switch (intr->intrinsic) {
         case nir_intrinsic_load_sample_id:
         case nir_intrinsic_load_sample_pos: sample_id = true; break;
         case nir_intrinsic_load_sample_mask_in: sample_mask = true; break;
         case nir_intrinsic_load_front_face: front_face = true; break;
         case nir_intrinsic_load_primitive_id: primitive_id = true; break;
         case nir_intrinsic_load_point_coord: point_coord = true; break;
         case nir_intrinsic_load_layer_id: layer = true; break;
         case nir_intrinsic_load_subgroup_size: subgroup_size = true; break;
         case nir_intrinsic_load_subgroup_invocation: subgroup_invocation = true; break;
         case nir_intrinsic_load_barycentric_coord_pixel:
         case nir_intrinsic_load_barycentric_coord_centroid:
         case nir_intrinsic_load_barycentric_coord_sample:
         case nir_intrinsic_load_barycentric_coord_at_offset:
         case nir_intrinsic_load_barycentric_coord_at_sample:
            barycentric[nir_intrinsic_interp_mode(intr) == INTERP_MODE_NOPERSPECTIVE] = true;
            break;
         default: break;
         }
         bool flat = intr->intrinsic == nir_intrinsic_load_input ||
                     intr->intrinsic == nir_intrinsic_load_per_primitive_input;
         if (!flat && intr->intrinsic != nir_intrinsic_load_interpolated_input &&
             !is_per_vertex_input(intr))
            continue;
         unsigned location = io_location(intr, flat ? 0 : 1);
         if (location == VARYING_SLOT_POS || location >= VARYING_SLOT_MAX)
            continue;
         unsigned bits = (nir_intrinsic_component(intr) + intr->def.num_components) *
                         intr->def.bit_size;
         width[location] = MAX2(width[location], DIV_ROUND_UP(bits, 32));
      }
   }
   /* msl_gather_info supplies SampleID for sample-position and mask reads. */
   sample_id |= sample_mask;
   if (prev) {
      nir_foreach_block(block, nir_shader_get_entrypoint(prev)) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_output)
               continue;
            unsigned location = io_location(intr, 1);
            if (!(fixed_outputs & BITFIELD64_BIT(location)))
               continue;
            width[location] = MAX2(width[location],
               DIV_ROUND_UP((nir_intrinsic_component(intr) + intr->num_components) *
                            intr->src[0].ssa->bit_size, 32));
         }
      }
   }
   /* Native mesh position contributes three additional packed interface
    * components on the measured Apple mesh path (124 generic reports127).
    * This is transport pressure, not a reduction of Vulkan output limits. */
   unsigned mesh_position = prev && prev->info.stage == MESA_SHADER_MESH ? 3u : 0u;
   unsigned components = mesh_position + sample_id + sample_mask + front_face +
      (primitive_id && !width[VARYING_SLOT_PRIMITIVE_ID]) +
      (layer && !width[VARYING_SLOT_LAYER]) + subgroup_size + subgroup_invocation +
      2 * point_coord + 3 * (barycentric[0] + barycentric[1]) +
      (prev ? prev->info.clip_distance_array_size : 0);
   for (unsigned i = 0; i < VARYING_SLOT_MAX; ++i)
      components += width[i];
   if (out_width)
      memcpy(out_width, width, sizeof(width));
   return components;
}

bool
kk_nir_needs_interface_packing(nir_shader *fs, nir_shader *prev)
{
   const uint64_t generic = BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
      ~BITFIELD64_MASK(VARYING_SLOT_VAR0);
   uint64_t fixed_outputs = (prev ? prev->info.outputs_written : 0) &
      ~(generic | VARYING_BIT_POS);
   if ((util_bitcount64(fs->info.inputs_read & ~VARYING_BIT_POS) +
        util_bitcount64(fixed_outputs)) * 4 +
       (prev ? prev->info.clip_distance_array_size : 0) + 32 <= 124)
      return false;
   return kk_native_interface_components(fs, prev, NULL) > 124;
}

static bool
is_barycentric_coord(nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_barycentric_coord_pixel:
   case nir_intrinsic_load_barycentric_coord_centroid:
   case nir_intrinsic_load_barycentric_coord_sample:
   case nir_intrinsic_load_barycentric_coord_at_sample:
   case nir_intrinsic_load_barycentric_coord_at_offset:
      return true;
   default:
      return false;
   }
}

enum kk_barycentric_usage
kk_nir_barycentric_usage(nir_shader *fs)
{
   /* Linked IO has current gather-info from fragment finishing. Ordinary
    * shaders need no instruction walk, even on a barycentric-enabled device.
    * Keep the full analysis for callers with pre-lowering metadata. */
   if (fs->info.io_lowered &&
       !BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PERSP_COORD) &&
       !BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_LINEAR_COORD))
      return KK_BARYCENTRIC_NONE;

   bool persp = false, linear = false;
   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (!is_barycentric_coord(intr))
            continue;
         if (intr->intrinsic != nir_intrinsic_load_barycentric_coord_pixel)
            return KK_BARYCENTRIC_EXACT;
         if (nir_intrinsic_interp_mode(intr) == INTERP_MODE_NOPERSPECTIVE)
            linear = true;
         else
            persp = true;
         if (persp && linear)
            return KK_BARYCENTRIC_EXACT;
      }
   }
   return (persp || linear) ? KK_BARYCENTRIC_PIXEL : KK_BARYCENTRIC_NONE;
}

static void
gather_flat_locations(nir_shader *fs, struct kk_per_vertex_map *map)
{
   uint64_t emulated = 0;
   u_foreach_bit64(location, map->locations) {
      for (unsigned v = 0u; v < 3u; ++v)
         emulated |= BITFIELD64_BIT(map->slot[location][v]);
   }
   for (unsigned i = 0u; i < 2u; ++i) {
      if (map->barycentric_slot[i])
         emulated |= BITFIELD64_BIT(map->barycentric_slot[i]);
   }
   if (map->buffer_slot)
      emulated |= BITFIELD64_BIT(map->buffer_slot);
   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_input &&
             intr->intrinsic != nir_intrinsic_load_per_primitive_input)
            continue;
         unsigned location = io_location(intr, 0);
         if (!(emulated & BITFIELD64_BIT(location)))
            map->flat_locations |= BITFIELD64_BIT(location);
      }
   }
}

static bool
alloc_slot(uint64_t *used, uint8_t *slot)
{
   for (unsigned s = VARYING_SLOT_VAR0; s <= VARYING_SLOT_VAR31; s++) {
      if (!(*used & BITFIELD64_BIT(s))) {
         *used |= BITFIELD64_BIT(s);
         *slot = s;
         return true;
      }
   }
   return false;
}

#include "kk_canonical_per_vertex.h"

static nir_def *load_per_draw(nir_builder *, unsigned, unsigned);
#include "kk_canonical_projected.h"

static nir_def *
load_per_vertex_corner(nir_builder *b, nir_intrinsic_instr *intr,
                       const struct kk_per_vertex_map *map, unsigned corner)
{
   unsigned location = io_location(intr, 1);
   return nir_load_input(
      b, intr->def.num_components, intr->def.bit_size, nir_imm_int(b, 0),
      .base = 0u, .component = nir_intrinsic_component(intr),
      .dest_type = nir_intrinsic_dest_type(intr),
      .io_semantics.location = map->slot[location][corner],
      .io_semantics.num_slots = 1u);
}

static nir_def *
load_per_vertex_value(nir_builder *b, nir_intrinsic_instr *intr,
                      const struct kk_per_vertex_map *map)
{
   if (nir_src_is_const(intr->src[0]))
      return load_per_vertex_corner(b, intr, map,
                                    MIN2(nir_src_as_uint(intr->src[0]), 2u));

   nir_def *values[3];
   for (unsigned v = 0u; v < 3u; ++v)
      values[v] = load_per_vertex_corner(b, intr, map, v);

   nir_def *vertex = intr->src[0].ssa;
   return nir_bcsel(
      b, nir_ieq_imm(b, vertex, 0), values[0],
      nir_bcsel(b, nir_ieq_imm(b, vertex, 1), values[1], values[2]));
}

/* With the emulation each triangle corner has its own vertex shader
 * invocation, which writes (1,0,0), (0,1,0) or (0,0,1). Interpolating that
 * smooth or noperspective gives the barycentrics at any position Metal can
 * interpolate at. */
static nir_def *
load_barycentric_value(nir_builder *b, nir_intrinsic_instr *intr,
                       const struct kk_per_vertex_map *map)
{
   if (map->projected_basis)
      return kk_canonical_projected_weights(b, intr);
   enum glsl_interp_mode mode = nir_intrinsic_interp_mode(intr);
   bool linear = mode == INTERP_MODE_NOPERSPECTIVE;
   if (!linear)
      mode = INTERP_MODE_SMOOTH;

   nir_def *bary;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_barycentric_coord_centroid:
      bary = nir_load_barycentric_centroid(b, 32, .interp_mode = mode);
      break;
   case nir_intrinsic_load_barycentric_coord_sample:
      /* This varying is inserted after the common sample-shading pass. */
      bary = nir_load_barycentric_at_sample(b, 32, nir_load_sample_id(b),
                                            .interp_mode = mode);
      break;
   case nir_intrinsic_load_barycentric_coord_at_sample:
      bary = nir_load_barycentric_at_sample(b, 32, intr->src[0].ssa,
                                            .interp_mode = mode);
      break;
   case nir_intrinsic_load_barycentric_coord_at_offset:
      bary = nir_load_barycentric_at_offset(b, 32, intr->src[0].ssa,
                                            .interp_mode = mode);
      break;
   default:
      bary = nir_load_barycentric_pixel(b, 32, .interp_mode = mode);
      break;
   }

   return nir_load_interpolated_input(
      b, 3, 32, bary, nir_imm_int(b, 0), .component = 0u,
      .dest_type = nir_type_float32,
      .io_semantics.location = map->barycentric_slot[linear],
      .io_semantics.num_slots = 1u);
}

/* Byte offset of a PerVertexKHR location of a corner in the buffer of a
 * primitive: corners one after the other, 16 bytes per location. */
static unsigned
buffer_offset(const struct kk_per_vertex_map *map, unsigned location,
              unsigned corner)
{
   if (map->canonical_heap)
      return kk_canonical_buffer_offset(location, corner);
   unsigned count = util_bitcount64(map->locations);
   unsigned index = util_bitcount64(map->locations & BITFIELD64_MASK(location));
   return (corner * count + index) * 16u;
}

static nir_def *load_per_draw(nir_builder *b, unsigned bit_size, unsigned offset);

static nir_def *
load_buffer_address(nir_builder *b, const struct kk_per_vertex_map *map)
{
   return nir_pack_64_2x32(
      b, nir_load_input(b, 2, 32, nir_imm_int(b, 0), .base = 0u,
                        .component = 0u, .dest_type = nir_type_uint32,
                        .io_semantics.location = map->buffer_slot,
                        .io_semantics.num_slots = 1u));
}

static nir_def *
load_buffer_corner_value(nir_builder *b, nir_intrinsic_instr *intr,
                          const struct kk_per_vertex_map *map,
                          unsigned location, nir_def *corner)
{
   unsigned stride = buffer_offset(map, location, 1) -
                     buffer_offset(map, location, 0);
   nir_def *addr = nir_iadd(b, load_buffer_address(b, map),
      nir_u2u64(b, nir_imul_imm(b, corner, stride)));
   return kk_nir_load_primitive_corner(b, addr,
      buffer_offset(map, location, 0) + nir_intrinsic_component(intr) * 4u,
      intr->def.num_components, intr->def.bit_size);
}

static void
load_buffer_corner_values(nir_builder *b, nir_intrinsic_instr *intr,
                          const struct kk_per_vertex_map *map,
                          unsigned location, nir_def *values[3])
{
   nir_def *addr = load_buffer_address(b, map);
   unsigned offset = nir_intrinsic_component(intr) * 4u;
   for (unsigned v = 0; v < 3; ++v) {
      values[v] = kk_nir_load_primitive_corner(b, addr,
         buffer_offset(map, location, v) + offset,
         intr->def.num_components, intr->def.bit_size);
   }
}

static nir_def *
load_per_vertex_buffer_value(nir_builder *b, nir_intrinsic_instr *intr,
                             const struct kk_per_vertex_map *map)
{
   /* Select the record address before reading it. A dynamic PerVertex index
    * needs one corner load, not three loads followed by a vector select. */
   nir_def *corner = nir_umin_imm(b, intr->src[0].ssa, 2u);
   return load_buffer_corner_value(b, intr, map, io_location(intr, 1), corner);
}

/* A full interface leaves no room for an address and basis varyings. Move
 * enough ordinary inputs through the same per-corner buffer, retaining their
 * original interpolation. One unused native slot leaves position headroom. */
static void
make_buffer_interface_room(nir_shader *fs, nir_shader *prev,
                           bool uses_barycentric[2],
                           struct kk_per_vertex_map *map)
{
   const uint64_t generic = BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
      ~BITFIELD64_MASK(VARYING_SLOT_VAR0);
   uint64_t used = (fs->info.inputs_read | prev->info.outputs_written) &
      ~map->unused_locations;
   uint8_t width[VARYING_SLOT_MAX];
   unsigned components = kk_native_interface_components(fs, prev, width);
   unsigned free = 32 - util_bitcount64(used & generic & ~map->locations);
   unsigned modes = uses_barycentric[0] + uses_barycentric[1];
   unsigned removed = 0;
   u_foreach_bit64(location, map->locations)
      removed += width[location];
   unsigned packed = components - removed + 2 + 3 * modes;
   if (free >= 2 + modes && packed <= 124)
      return;

   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         bool interpolated = intr->intrinsic == nir_intrinsic_load_interpolated_input;
         if (!interpolated && intr->intrinsic != nir_intrinsic_load_input &&
             intr->intrinsic != nir_intrinsic_load_per_primitive_input)
            continue;
         unsigned location = io_location(intr, interpolated ? 1 : 0);
         uint64_t bit = BITFIELD64_BIT(location);
         if (!(generic & bit) || (map->locations & bit))
            continue;
         map->locations |= bit;
         map->buffered_locations |= bit;
         if (interpolated) {
            nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
            uses_barycentric[nir_intrinsic_interp_mode(bary) ==
                               INTERP_MODE_NOPERSPECTIVE] = true;
         }
         /* Only moving an input can change the native interface budget. */
         free += (used & bit) != 0;
         removed += width[location];
         modes = uses_barycentric[0] + uses_barycentric[1];
         packed = components - removed + 2 + 3 * modes;
         if (free >= 2 + modes && packed <= 124)
            return;
      }
   }
}

static nir_def *
load_buffered_input(nir_builder *b, nir_intrinsic_instr *intr,
                     const struct kk_per_vertex_map *map)
{
   bool interpolated = intr->intrinsic == nir_intrinsic_load_interpolated_input;
   if (!interpolated) {
      nir_def *last = load_per_draw(b, 32,
         offsetof(struct kk_per_draw_data, provoking_last));
      nir_def *topology = load_per_draw(b, 32,
         offsetof(struct kk_per_draw_data, per_vertex_topology));
      nir_def *corner = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_POINTS),
         nir_imm_int(b, 0), nir_bcsel(b,
            nir_ior(b, nir_ieq_imm(b, topology, MESA_PRIM_LINES),
                       nir_ieq_imm(b, topology, MESA_PRIM_LINE_STRIP)),
            nir_imm_int(b, 1), nir_imm_int(b, 2)));
      corner = nir_bcsel(b, nir_ine_imm(b, last, 0), corner, nir_imm_int(b, 0));
      return load_buffer_corner_value(b, intr, map, io_location(intr, 0), corner);
   }
   nir_def *values[3];
   load_buffer_corner_values(b, intr, map, io_location(intr, 1), values);
   nir_intrinsic_instr *original = nir_src_as_intrinsic(intr->src[0]);
   enum glsl_interp_mode mode = nir_intrinsic_interp_mode(original);
   nir_def *interpolation = intr->src[0].ssa;
   if (original->intrinsic == nir_intrinsic_load_barycentric_sample)
      interpolation = nir_load_barycentric_at_sample(
         b, 32, nir_load_sample_id(b), .interp_mode = mode);
   nir_def *weights = map->projected_basis ?
      kk_canonical_projected_weights(b, original) : nir_load_interpolated_input(
      b, 3, 32, interpolation, nir_imm_int(b, 0), .dest_type = nir_type_float32,
      .io_semantics.location = map->barycentric_slot[mode == INTERP_MODE_NOPERSPECTIVE],
      .io_semantics.num_slots = 1);
   for (unsigned i = 0; i < 3; ++i)
      values[i] = nir_fmul(b, nir_f2f32(b, values[i]), nir_channel(b, weights, i));
   return nir_f2fN(b, nir_fadd(b, nir_fadd(b, values[0], values[1]), values[2]),
                    intr->def.bit_size);
}

bool
kk_nir_apply_fs_per_vertex_map(nir_shader *fs, struct kk_per_vertex_map *map)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(fs);
   nir_builder b = nir_builder_create(impl);
   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      b.impl = body;
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         b.cursor = nir_before_instr(instr);
         if (is_per_vertex_input(intr) && map->buffer) {
            nir_def_replace(&intr->def,
                            load_per_vertex_buffer_value(&b, intr, map));
         } else if (is_per_vertex_input(intr)) {
            nir_def_replace(&intr->def, load_per_vertex_value(&b, intr, map));
         } else if ((intr->intrinsic == nir_intrinsic_load_input ||
                     intr->intrinsic == nir_intrinsic_load_per_primitive_input ||
                     intr->intrinsic == nir_intrinsic_load_interpolated_input) &&
                    (map->buffered_locations & BITFIELD64_BIT(io_location(intr,
                       intr->intrinsic == nir_intrinsic_load_interpolated_input ? 1 : 0)))) {
            nir_def_replace(&intr->def, load_buffered_input(&b, intr, map));
         } else if (is_barycentric_coord(intr)) {
            nir_def_replace(&intr->def, load_barycentric_value(&b, intr, map));
         }
      }
   }

   map->active = true;
   gather_flat_locations(fs, map);
   nir_foreach_function_impl(body, fs)
      nir_progress(true, body, nir_metadata_control_flow);
   nir_shader_gather_info(fs, impl);
   return true;
}

bool
kk_nir_lower_fs_per_vertex_inputs(nir_shader *fs, nir_shader *prev, bool force,
                                  struct kk_per_vertex_map *map)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(fs);
   bool full_interface = kk_nir_needs_interface_packing(fs, prev);
   if (!force && !full_interface)
      return nir_no_progress(impl);
   bool projected = map->projected_basis;
   memset(map, 0, sizeof(*map));
   bool uses_barycentric[2] = {false, false};
   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (is_per_vertex_input(intr))
            map->locations |= BITFIELD64_BIT(io_location(intr, 1));
         else if (is_barycentric_coord(intr))
            uses_barycentric[nir_intrinsic_interp_mode(intr) ==
                             INTERP_MODE_NOPERSPECTIVE] = true;
      }
   }

   nir_shader_gather_info(fs, impl);
   if (!prev) {
      kk_canonical_fs_map(fs, map);
   } else {
      nir_shader_gather_info(prev, nir_shader_get_entrypoint(prev));
      /* Only the final native rasterization VS drops these stores. Earlier
       * compute/Poly passes retain the complete capture and XFB interface. */
      const uint64_t generic = BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
         ~BITFIELD64_MASK(VARYING_SLOT_VAR0);
      map->unused_locations = prev->info.outputs_written & generic & ~fs->info.inputs_read;
      uint64_t used = (fs->info.inputs_read | prev->info.outputs_written | map->locations) &
         ~map->unused_locations;

      bool ok = true;
      u_foreach_bit64(location, map->locations) {
         map->slot[location][0] = location;
         ok &= alloc_slot(&used, &map->slot[location][1]);
         ok &= alloc_slot(&used, &map->slot[location][2]);
      }
      for (unsigned i = 0u; i < 2u; ++i) {
         if (uses_barycentric[i])
            ok &= alloc_slot(&used, &map->barycentric_slot[i]);
      }
      if (!ok || full_interface || KK_DEBUG(FORCE_PER_VERTEX_BUFFER)) {
         uint64_t locations = map->locations;
         uint64_t unused = map->unused_locations;
         memset(map, 0, sizeof(*map));
         map->unused_locations = unused;
         map->locations = locations;
         map->buffer = true;
         make_buffer_interface_room(fs, prev, uses_barycentric, map);
         locations = map->locations;
         /* The PerVertexKHR locations are read from the buffer, so only the
          * address needs a slot. */
         used = (fs->info.inputs_read | prev->info.outputs_written) &
            ~(locations | map->unused_locations);
         if (locations && !alloc_slot(&used, &map->buffer_slot)) {
            memset(map, 0, sizeof(*map));
            map->out_of_slots = true;
            return nir_no_progress(impl);
         }
         /* Buffering frees the original per-vertex locations. Keep native
          * interpolants for the weights whenever those slots are available;
          * Both weights are evaluated through the same native interpolants. */
         for (unsigned i = 0; i < 2; ++i) {
            if (uses_barycentric[i])
               if (!alloc_slot(&used, &map->barycentric_slot[i])) {
                  map->out_of_slots = true;
                  return nir_no_progress(impl);
               }
         }
      }

   }

   if (projected) {
      map->projected_basis = true;
      map->barycentric_slot[0] = map->barycentric_slot[1] = KK_CANONICAL_PROJECTED_POINT;
   }
   return kk_nir_apply_fs_per_vertex_map(fs, map);
}

static nir_def *
load_per_draw(nir_builder *b, unsigned bit_size, unsigned offset)
{
   nir_def *addr = nir_iadd_imm(b, nir_load_per_draw_ptr_kk(b, 1, 64), offset);
   return nir_build_load_global_constant(b, 1, bit_size, addr,
                                         .align_mul = bit_size / 8u,
                                         .access = ACCESS_CAN_SPECULATE);
}

#define per_draw_offset(member) offsetof(struct kk_per_draw_data, member)

/* Returns the Vulkan vertex index of the given position in the draw. Indexed
 * draws are issued as non-indexed so the index buffer is read here. Like
 * poly_load_index, out-of-bounds positions read the first element.
 */
static nir_def *
vertex_index(nir_builder *b, nir_def *position, nir_def *index_addr,
             nir_def *index_size, nir_def *index_count, nir_def *base_vertex)
{
   nir_def *indexed = nir_ine_imm(b, index_addr, 0);
   nir_push_if(b, indexed);
   nir_def *element = nir_bcsel(b, nir_uge(b, position, index_count),
                                nir_imm_int(b, 0), position);
   nir_def *addr =
      nir_iadd(b, index_addr, nir_u2u64(b, nir_imul(b, element, index_size)));
   nir_def *aligned = nir_iand_imm(b, addr, ~3ull);
   nir_def *word =
      nir_build_load_global_constant(b, 1, 32, aligned, .align_mul = 4u);
   nir_def *shift = nir_imul_imm(b, nir_u2u32(b, nir_iand_imm(b, addr, 3)), 8);
   word = nir_ushr(b, word, shift);
   nir_def *mask =
      nir_bcsel(b, nir_ieq_imm(b, index_size, 4), nir_imm_int(b, ~0),
                nir_bcsel(b, nir_ieq_imm(b, index_size, 2),
                          nir_imm_int(b, 0xffff), nir_imm_int(b, 0xff)));
   nir_def *index = nir_iadd(b, nir_iand(b, word, mask), base_vertex);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, index, position);
}

static bool
is_side_effect(nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_store_global:
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_atomic:
   case nir_intrinsic_image_atomic_swap:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
   case nir_intrinsic_store_clip_distance_kk:
      return true;
   default:
      return false;
   }
}

static nir_def *
load_param(nir_shader *nir, nir_instr *before, unsigned idx)
{
   nir_intrinsic_instr *param =
      nir_intrinsic_instr_create(nir, nir_intrinsic_load_param);
   nir_intrinsic_set_param_idx(param, idx);
   param->num_components = 1;
   nir_def_init(&param->instr, &param->def, 1, idx == 2u ? 64 : 32);
   nir_instr_insert_before(before, &param->instr);
   return &param->def;
}

/* Turns a copy of the vertex shader into one that only writes the outputs
 * of a given triangle corner: its per-vertex outputs, and its flat outputs if
 * it is the provoking corner. Parameter 0 is the vertex index, parameter 1 is
 * non-zero for the provoking corner, parameter 2 is the buffer the per-vertex
 * outputs are written to without varying slots for them, 0 if none.
 */
static void
make_corner_template(nir_shader *nir, nir_function_impl *impl,
                     const struct kk_per_vertex_map *map, unsigned corner)
{
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (intr->intrinsic == nir_intrinsic_load_vertex_id) {
            nir_def_replace(&intr->def, load_param(nir, instr, 0));
         } else if (intr->intrinsic == nir_intrinsic_store_output) {
            unsigned location = io_location(intr, 1);
            if (map->projected_basis && location == VARYING_SLOT_POS) {
               nir_builder b = nir_builder_at(nir_before_instr(instr));
               kk_canonical_store_corner(&b, intr, corner, load_param(nir, instr, 2));
               nir_instr_remove(instr);
            } else if ((map->locations & BITFIELD64_BIT(location)) && map->buffer) {
               nir_def *buffer = load_param(nir, instr, 2);
               nir_builder b = nir_builder_at(nir_before_instr(instr));
               nir_def *value = intr->src[0].ssa;
               if (value->bit_size != 32)
                  value = nir_u2u32(&b, value);
               unsigned offset = buffer_offset(map, location, corner) +
                                 nir_intrinsic_component(intr) * 4u;
               nir_push_if(&b, nir_ine_imm(&b, buffer, 0));
               nir_build_store_global(
                  &b, value, nir_iadd_imm(&b, buffer, offset),
                  .write_mask = nir_intrinsic_write_mask(intr),
                  .align_mul = 4u);
               nir_pop_if(&b, NULL);
               nir_instr_remove(instr);
            } else if (map->locations & BITFIELD64_BIT(location)) {
               nir_io_semantics io = nir_intrinsic_io_semantics(intr);
               io.location = map->slot[location][corner];
               io.num_slots = 1u;
               nir_intrinsic_set_io_semantics(intr, io);
               nir_builder b = nir_builder_at(nir_before_instr(instr));
               nir_src_rewrite(&intr->src[1], nir_imm_int(&b, 0));
            } else if (map->flat_locations & BITFIELD64_BIT(location)) {
               nir_def *provoking = load_param(nir, instr, 1);
               nir_builder b = nir_builder_at(nir_before_instr(instr));
               nir_push_if(&b, nir_ine_imm(&b, provoking, 0));
               nir_instr_remove(instr);
               nir_builder_instr_insert(&b, instr);
               nir_pop_if(&b, NULL);
            } else {
               nir_instr_remove(instr);
            }
         } else if (is_side_effect(intr)) {
            if (nir_intrinsic_infos[intr->intrinsic].has_dest) {
               nir_builder b = nir_builder_at(nir_before_instr(instr));
               nir_def_replace(
                  &intr->def,
                  nir_undef(&b, intr->def.num_components, intr->def.bit_size));
            } else {
               nir_instr_remove(instr);
            }
         }
      }
   }
}

struct topology_values {
   nir_def *points, *lines, *line_strip, *triangles, *triangle_strip,
      *triangle_fan;
};

static nir_def *
select_by_topology(nir_builder *b, nir_def *topology,
                   const struct topology_values *v)
{
   nir_def *r = v->triangles;
   r = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_TRIANGLE_STRIP),
                 v->triangle_strip, r);
   r = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_TRIANGLE_FAN),
                 v->triangle_fan, r);
   r = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_POINTS), v->points, r);
   r = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_LINES), v->lines, r);
   r = nir_bcsel(b, nir_ieq_imm(b, topology, MESA_PRIM_LINE_STRIP),
                 v->line_strip, r);
   return r;
}

bool
kk_nir_lower_vs_per_vertex_outputs_with_state(
   nir_shader *nir, const struct kk_per_vertex_map *map,
   const struct vk_graphics_pipeline_state *state)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct kk_per_vertex_map canonical;
   if (map && map->canonical_heap) {
      nir_shader_gather_info(nir, impl);
      kk_canonical_map_init(&canonical);
      canonical.projected_basis = map->projected_basis;
      if (canonical.projected_basis)
         canonical.barycentric_slot[0] = canonical.barycentric_slot[1] = KK_CANONICAL_PROJECTED_POINT;
      canonical.locations = nir->info.outputs_written & kk_canonical_generic_mask();
      map = &canonical;
   }
   if (!map || !map->active)
      return nir_no_progress(impl);

   /* Basis-only shaders need no original values from the other corners.
    * Flat-only shaders need just the provoking corner, not three guarded
    * copies of its body. Keep three templates for real per-corner transport. */
   bool flat_only = !map->locations && !map->projected_basis;
   unsigned template_count = flat_only ? (map->flat_locations ? 1u : 0u) : 3u;
   nir_function_impl *templates[3];
   nir_function *corner_function = NULL;
   if (template_count) {
      corner_function = nir_function_create(nir, "kk_vs_corner");
      corner_function->num_params = 3;
      corner_function->params = rzalloc_array(nir, nir_parameter, 3);
      for (unsigned i = 0u; i < 3u; ++i)
         corner_function->params[i] =
            (nir_parameter){.num_components = 1, .bit_size = i == 2u ? 64 : 32};
   }

   for (unsigned v = 0u; v < template_count; ++v) {
      templates[v] = nir_function_impl_clone(nir, impl);
      templates[v]->function = corner_function;
      make_corner_template(nir, templates[v], map, v);
   }

   nir_builder b = nir_builder_at(nir_before_impl(impl));
   nir_def *hw_vertex_load = nir_load_vertex_id(&b);
   nir_def *hw_vertex_id = hw_vertex_load;
   nir_def *index_addr =
      load_per_draw(&b, 64, per_draw_offset(per_vertex_index_addr));
   nir_def *index_size =
      load_per_draw(&b, 32, per_draw_offset(per_vertex_index_size));
   nir_def *index_count =
      load_per_draw(&b, 32, per_draw_offset(per_vertex_index_count));
   nir_def *first_vertex =
      load_per_draw(&b, 32, per_draw_offset(per_vertex_first_vertex));
   nir_def *base_vertex =
      load_per_draw(&b, 32, per_draw_offset(per_vertex_base_vertex));
   /* The draw ABI also serves dynamic and independent producers. A linked
    * ordinary VS already has these values in its pipeline key: specialize
    * here so constant folding removes the generic topology selects and
    * runtime primitive-size division. Canonical producers keep the ABI. */
   bool static_topology = state && state->ia && !map->canonical_heap &&
      !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY);
   unsigned static_prim_vertices = 0;
   if (static_topology) {
      switch (state->ia->primitive_topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
         static_prim_vertices = 1;
         break;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
         static_prim_vertices = 2;
         break;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
         static_prim_vertices = 3;
         break;
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
         /* Restart preserves primitive size even when it changes topology. */
         static_prim_vertices = state->ia->primitive_topology ==
            VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ? 2 : 3;
         /* Indexed restart is unrolled to lists before the corner draw.
          * The input topology is fixed only when that rewrite cannot occur. */
         static_topology = !state->ia->primitive_restart_enable &&
            !BITSET_TEST(state->dynamic,
                         MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE);
         break;
      default:
         static_topology = false;
         break;
      }
   }
   nir_def *topology = static_topology
      ? nir_imm_int(&b, vk_topology_to_mesa(state->ia->primitive_topology))
      : load_per_draw(&b, 32, per_draw_offset(per_vertex_topology));
   bool static_provoking = state && state->rs && !map->canonical_heap &&
      !BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX);
   nir_def *provoking_last = static_provoking
      ? nir_imm_bool(&b, state->rs->provoking_vertex ==
                           VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT)
      : nir_ine_imm(&b, load_per_draw(&b, 32, per_draw_offset(provoking_last)), 0);

   /* Indirect draws: firstVertex/firstIndex and vertexOffset are at the same
    * place in both command layouts. */
   nir_def *indirect =
      load_per_draw(&b, 64, per_draw_offset(per_vertex_indirect_addr));
   nir_push_if(&b, nir_ine_imm(&b, indirect, 0));
   nir_def *indirect_first = nir_build_load_global_constant(
      &b, 1, 32, nir_iadd_imm(&b, indirect, 8), .align_mul = 4u);
   nir_def *indirect_base =
      nir_bcsel(&b, nir_ine_imm(&b, index_addr, 0),
                nir_build_load_global_constant(
                   &b, 1, 32, nir_iadd_imm(&b, indirect, 12), .align_mul = 4u),
                nir_imm_int(&b, 0));
   nir_pop_if(&b, NULL);
   first_vertex = nir_if_phi(&b, indirect_first, first_vertex);
   base_vertex = nir_if_phi(&b, indirect_base, base_vertex);

   /* Original indirect commands start at their first vertex or index, plus
    * the vertex offset for indexed ones. */
   nir_def *raw = nir_ine_imm(
      &b, load_per_draw(&b, 32, per_draw_offset(per_vertex_raw_indirect)), 0);
   hw_vertex_id = nir_bcsel(
      &b, raw,
      nir_isub(&b, nir_isub(&b, hw_vertex_id, first_vertex), base_vertex),
      hw_vertex_id);

   /* The draw is a non-indexed point, line or triangle list starting at
    * vertex 0. */
   nir_def *one = nir_imm_int(&b, 1);
   nir_def *two = nir_imm_int(&b, 2);
   nir_def *three = nir_imm_int(&b, 3);
   nir_def *prim_vertices = static_prim_vertices
      ? nir_imm_int(&b, static_prim_vertices)
      : select_by_topology(&b, topology,
          &(struct topology_values){one, two, two, three, three, three});
   nir_def *triangle = nir_udiv(&b, hw_vertex_id, prim_vertices);
   nir_def *corner = nir_umod(&b, hw_vertex_id, prim_vertices);
   nir_def *odd = nir_ine_imm(&b, nir_iand_imm(&b, triangle, 1), 0);
   nir_def *t1 = nir_iadd_imm(&b, triangle, 1);
   nir_def *t2 = nir_iadd_imm(&b, triangle, 2);
   nir_def *zero = nir_imm_int(&b, 0);

   /* Vertex order of the "Barycentric Interpolation" section, which depends
    * on the provoking vertex mode for strips and fans. */
   nir_def *list_base = nir_imul_imm(&b, triangle, 3);
   nir_def *strip_first[3] = {triangle, nir_bcsel(&b, odd, t2, t1),
                              nir_bcsel(&b, odd, t1, t2)};
   nir_def *strip_last[3] = {nir_bcsel(&b, odd, t1, triangle),
                             nir_bcsel(&b, odd, triangle, t1), t2};
   nir_def *strip[3];
   for (unsigned v = 0u; v < 3u; ++v)
      strip[v] = nir_bcsel(&b, provoking_last, strip_last[v], strip_first[v]);
   nir_def *fan[3] = {nir_bcsel(&b, provoking_last, zero, t1),
                      nir_bcsel(&b, provoking_last, t1, t2),
                      nir_bcsel(&b, provoking_last, t2, zero)};

   /* Points and lines repeat their last vertex. */
   nir_def *line_base = nir_imul_imm(&b, triangle, 2);
   nir_def *positions[3], *indices[3];
   for (unsigned v = 0u; v < 3u; ++v) {
      unsigned line_vertex = MIN2(v, 1u);
      nir_def *position =
         select_by_topology(&b, topology,
                            &(struct topology_values){
                               triangle,
                               nir_iadd_imm(&b, line_base, line_vertex),
                               nir_iadd_imm(&b, triangle, line_vertex),
                               nir_iadd_imm(&b, list_base, v),
                               strip[v],
                               fan[v],
                            });
      positions[v] = position;
      if (template_count == 3u) {
         indices[v] =
            vertex_index(&b, nir_iadd(&b, first_vertex, position), index_addr,
                         index_size, index_count, base_vertex);
      }
   }
   nir_def *own_index;
   if (template_count == 3u) {
      own_index = nir_bcsel(
         &b, nir_ieq_imm(&b, corner, 0), indices[0],
         nir_bcsel(&b, nir_ieq_imm(&b, corner, 1), indices[1], indices[2]));
   } else {
      /* Select before decoding the index buffer when only this invocation
       * and, for flat inputs, the provoking corner need original values. */
      nir_def *position = nir_bcsel(
         &b, nir_ieq_imm(&b, corner, 0), positions[0],
         nir_bcsel(&b, nir_ieq_imm(&b, corner, 1), positions[1], positions[2]));
      own_index =
         vertex_index(&b, nir_iadd(&b, first_vertex, position), index_addr,
                      index_size, index_count, base_vertex);
   }

   /* With this order the Vulkan provoking vertex is always the first or the
    * last corner of the emitted primitive. */
   nir_def *provoking_corner =
      nir_bcsel(&b, provoking_last, nir_iadd_imm(&b, prim_vertices, -1), zero);
   nir_def *flat_index = NULL;
   if (template_count == 1u) {
      nir_def *position = nir_bcsel(
         &b, nir_ieq_imm(&b, provoking_corner, 0), positions[0],
         nir_bcsel(&b, nir_ieq_imm(&b, provoking_corner, 1), positions[1], positions[2]));
      flat_index =
         vertex_index(&b, nir_iadd(&b, first_vertex, position), index_addr,
                      index_size, index_count, base_vertex);
   }

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_load_vertex_id &&
             &intr->def != hw_vertex_load) {
            nir_def_replace(&intr->def, own_index);
         } else if (map->projected_basis && intr->intrinsic == nir_intrinsic_store_output &&
                    io_location(intr, 1) == VARYING_SLOT_POS) {
            nir_builder carrier = nir_builder_at(nir_before_instr(instr));
            kk_canonical_store_position(&carrier, intr, KK_CANONICAL_PROJECTED_POINT);
         } else if (intr->intrinsic == nir_intrinsic_store_output &&
                    ((map->locations | map->flat_locations | map->unused_locations) &
                     BITFIELD64_BIT(io_location(intr, 1)))) {
            nir_instr_remove(instr);
         }
      }
   }

   b.cursor = nir_after_impl(impl);

   /* Without varying slots for them, the first corner allocates the buffer
    * the per-vertex outputs of all corners go to. Its flat address output
    * is the one Metal rasterizes, the other corners write 0. If the heap is
    * full, the values go to its start and are wrong. */
   nir_def *buffer = nir_imm_int64(&b, 0);
   if (map->buffer && map->locations) {
      unsigned count = map->canonical_heap ? KK_CANONICAL_VARYING_COUNT
                                           : util_bitcount64(map->locations);
      unsigned size = map->projected_basis ? kk_canonical_projected_buffer_size() : 3u * count * 16u;
      nir_def *heap = load_per_draw(&b, 64, per_draw_offset(per_vertex_heap));
      nir_push_if(&b, nir_iand(&b, nir_ieq_imm(&b, corner, 0),
                               nir_ine_imm(&b, heap, 0)));
      nir_def *bottom = nir_global_atomic(
         &b, 32, nir_iadd_imm(&b, heap, offsetof(struct poly_heap, bottom)),
         nir_imm_int(&b, size + 16u), .atomic_op = nir_atomic_op_iadd);
      nir_def *offset = nir_iand_imm(&b, nir_iadd_imm(&b, bottom, 15), ~15u);
      nir_def *heap_size = nir_build_load_global(
         &b, 1, 32, nir_iadd_imm(&b, heap, offsetof(struct poly_heap, size)),
         .align_mul = 4u);
      offset =
         nir_bcsel(&b, nir_ult(&b, heap_size, nir_iadd_imm(&b, offset, size)),
                   nir_imm_int(&b, 0), offset);
      nir_def *base = nir_build_load_global(
         &b, 1, 64, nir_iadd_imm(&b, heap, offsetof(struct poly_heap, base)),
         .align_mul = 8u);
      nir_def *allocated = nir_iadd(&b, base, nir_u2u64(&b, offset));
      nir_pop_if(&b, NULL);
      buffer = nir_if_phi(&b, allocated, buffer);

      nir_store_output(&b, nir_unpack_64_2x32(&b, buffer), nir_imm_int(&b, 0),
                       .base = 0u, .range = 1u, .write_mask = 0x3,
                       .component = 0u, .src_type = nir_type_uint32,
                       .io_semantics.location = map->buffer_slot,
                       .io_semantics.num_slots = 1u);
   }

   if (!map->projected_basis &&
       (map->barycentric_slot[0] || map->barycentric_slot[1])) {
      nir_def *basis = nir_vec3(&b, nir_b2f32(&b, nir_ieq_imm(&b, corner, 0)),
                                nir_b2f32(&b, nir_ieq_imm(&b, corner, 1)),
                                nir_b2f32(&b, nir_ieq_imm(&b, corner, 2)));
      for (unsigned i = 0u; i < 2u; ++i) {
         if (!map->barycentric_slot[i])
            continue;
         nir_store_output(&b, basis, nir_imm_int(&b, 0), .base = 0u, .range = 1u,
                          .write_mask = 0x7, .component = 0u,
                          .src_type = nir_type_float32,
                          .io_semantics.location = map->barycentric_slot[i],
                          .io_semantics.num_slots = 1u);
      }
   }
   for (unsigned v = 0u; v < template_count; ++v) {
      nir_def *index = flat_only ? flat_index : indices[v];
      nir_def *params[3] = {
         index,
         flat_only ? nir_imm_int(&b, 1) :
            nir_b2i32(&b, nir_ieq_imm(&b, provoking_corner, v)),
         buffer,
      };
      nir_inline_function_impl(&b, templates[v], params, NULL);
   }
   nir_remove_non_entrypoints(nir);
   nir_index_ssa_defs(impl);

   nir->info.outputs_written = 0;
   nir_progress(true, impl, nir_metadata_none);
   nir_shader_gather_info(nir, impl);
   return true;
}

bool
kk_nir_lower_vs_per_vertex_outputs(nir_shader *nir,
                                   const struct kk_per_vertex_map *map)
{
   return kk_nir_lower_vs_per_vertex_outputs_with_state(nir, map, NULL);
}

/* Mesh outputs are already staged after its collective body. Flat interfaces
 * need the shared address packet, without the VS producer's corner replay. */
bool
kk_nir_lower_mesh_flat_inputs(nir_shader *fs, nir_shader *mesh,
                              struct kk_per_vertex_map *map)
{
   assert(mesh->info.stage == MESA_SHADER_MESH);
   if (!kk_nir_needs_interface_packing(fs, mesh))
      return false;
   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (is_barycentric_coord(intr) || is_per_vertex_input(intr) ||
             (intr->intrinsic == nir_intrinsic_load_interpolated_input &&
              (kk_canonical_generic_mask() & BITFIELD64_BIT(io_location(intr, 1)))))
            map->projected_basis = true;
      }
   }
   return kk_nir_lower_fs_per_vertex_inputs(fs, NULL, true, map);
}
