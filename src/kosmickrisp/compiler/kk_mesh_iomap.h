/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_IOMAP_H
#define KK_MESH_IOMAP_H
#include "kosmickrisp/libkk/kk_canonical_layout.h"

static bool
kk_mesh_gather_io(nir_intrinsic_instr *intr, struct io_slot_info *outputs)
{
   bool store;
   unsigned offset_src;
   const bool primitive = intr->intrinsic == nir_intrinsic_store_per_primitive_output ||
                          intr->intrinsic == nir_intrinsic_load_per_primitive_output;
   switch (intr->intrinsic) {
   case nir_intrinsic_store_per_vertex_output:
   case nir_intrinsic_store_per_primitive_output:
      store = true;
      offset_src = 2;
      break;
   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_load_per_primitive_output:
      store = false;
      offset_src = 1;
      break;
   default:
      return false;
   }
   nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);
   unsigned first = io.location;
   unsigned count = io.num_slots;
   if (nir_src_is_const(intr->src[offset_src])) {
      first += nir_src_as_uint(intr->src[offset_src]);
      count = 1;
   }
   for (unsigned i = first; i < first + count; ++i) {
      if (i >= NUM_TOTAL_VARYING_SLOTS)
         break;
      outputs[i].type = store ? nir_intrinsic_src_type(intr) :
                                nir_intrinsic_dest_type(intr);
      outputs[i].num_components = MAX2(outputs[i].num_components,
                                       component + intr->num_components);
      outputs[i].mesh_primitive |= primitive;
      outputs[i].mesh_vertex |= !primitive;
   }
   return true;
}

/* Slots accessed after the last info gather, such as the second slot of a
 * 64-bit vector output split into 32-bit halves, take their class from the
 * accessing intrinsics so every block declares them. */
static void
kk_mesh_gather_masks(nir_shader *s, const struct io_slot_info *outputs)
{
   for (unsigned location = 0; location < 64; ++location) {
      const struct io_slot_info *info = &outputs[location];
      if (info->mesh_vertex == info->mesh_primitive)
         continue;
      const uint64_t bit = BITFIELD64_BIT(location);
      if (!((s->info.outputs_written | s->info.outputs_read) & bit))
         s->info.outputs_written |= bit;
      if (info->mesh_primitive)
         s->info.per_primitive_outputs |= bit;
      else
         s->info.per_primitive_outputs &= ~bit;
   }
}

/* Mirror the emitted native (not packed staging) field layout. A vector3 has
 * vector4 size/alignment; scalarized builtins are emitted as separate fields.
 * The generated MSL asserts equality with sizeof, not an API-size cutoff. */
static void
kk_mesh_layout_field(unsigned *size, unsigned *alignment,
                     unsigned bytes, unsigned align)
{
   *size = ALIGN_POT(*size, align) + bytes;
   *alignment = MAX2(*alignment, align);
}

static unsigned
kk_mesh_native_layout(struct nir_to_msl_ctx *ctx, uint64_t mask, bool vertex)
{
   unsigned size = 0, alignment = 1;
   if (ctx->mesh.flat_heap) {
      mask &= ~(BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
                ~BITFIELD64_MASK(VARYING_SLOT_VAR0));
      if (!vertex) kk_mesh_layout_field(&size, &alignment, 8, 8);
      if (vertex && ctx->mesh.projected_basis)
         kk_mesh_layout_field(&size, &alignment, 16, 16);
   }
   if (vertex && !(mask & VARYING_BIT_POS))
      kk_mesh_layout_field(&size, &alignment, 16, 16);
   u_foreach_bit64(location, mask) {
      struct io_slot_info info = ctx->outputs_info[location];
      if (!info.num_components) continue;
      unsigned scalar = location == VARYING_SLOT_CULL_PRIMITIVE ? 1u :
         (location == VARYING_SLOT_LAYER || location == VARYING_SLOT_VIEWPORT) ? 4u :
         nir_alu_type_get_type_size(info.type) / 8u;
      unsigned n = info.num_components;
      unsigned field_offset = ALIGN_POT(size, VARYING_SLOT_INFO[location].scalarized
                                            ? scalar : scalar * (n == 3u ? 4u : n));
      if (VARYING_SLOT_INFO[location].scalarized) {
         for (unsigned c = 0; c < n; ++c)
            kk_mesh_layout_field(&size, &alignment, scalar, scalar);
      } else {
         unsigned bytes = scalar * (n == 3u ? 4u : n);
         kk_mesh_layout_field(&size, &alignment, bytes, bytes);
      }
      unsigned field = location == VARYING_SLOT_PRIMITIVE_ID ? 0u :
         location == VARYING_SLOT_PRIMITIVE_SHADING_RATE ? 1u :
         location == VARYING_SLOT_LAYER ? 2u : 4u;
      if (field < 4u) {
         ctx->mesh.retained_fields[field][0] = vertex ? 1u : 2u;
         ctx->mesh.retained_fields[field][1] = field_offset;
         ctx->mesh.retained_fields[field][2] = location == VARYING_SLOT_LAYER
            ? nir_type_uint32 : info.type;
         ctx->mesh.retained_fields[field][3] = n;
      }
   }
   if (vertex && ctx->shader->info.clip_distance_array_size)
      kk_mesh_layout_field(&size, &alignment,
         4u * ctx->shader->info.clip_distance_array_size, 4u);
   if (!vertex && ctx->shader->info.cull_distance_array_size &&
       !(mask & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE)))
      kk_mesh_layout_field(&size, &alignment, 1u, 1u);
   if (!vertex && ctx->mesh.view_mask && !(mask & VARYING_BIT_LAYER)) {
      ctx->mesh.retained_fields[2][0] = 2u;
      ctx->mesh.retained_fields[2][1] = ALIGN_POT(size, 4u);
      ctx->mesh.retained_fields[2][2] = nir_type_uint32;
      ctx->mesh.retained_fields[2][3] = 1u;
      kk_mesh_layout_field(&size, &alignment, 4u, 4u);
   }
   if (!vertex && !ctx->shader->info.cull_distance_array_size &&
       !(mask & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE)))
      kk_mesh_layout_field(&size, &alignment, 1u, 1u);
   return ALIGN_POT(MAX2(size, 1u), alignment);
}

static void
kk_mesh_io_block(struct nir_to_msl_ctx *ctx, const char *name, uint64_t mask,
                  bool vertex, bool storage)
{
   P(ctx, "struct %s {\n", name);
   ctx->indentlevel++;
   if (!storage && ctx->mesh.flat_heap) {
      mask &= ~(BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
                ~BITFIELD64_MASK(VARYING_SLOT_VAR0));
      if (!vertex) {
         P_IND(ctx, "uint2 ");
         varying_slot_name(ctx, VARYING_SLOT_VAR0, 0);
         P(ctx, " ");
         varying_slot_semantic(ctx, VARYING_SLOT_VAR0, 0);
         P(ctx, ";\n");
      }
      if (ctx->mesh.projected_basis && vertex) {
         P_IND(ctx, "float3 ");
         varying_slot_name(ctx, KK_CANONICAL_PROJECTED_POINT, 0);
         P(ctx, " ");
         varying_slot_semantic(ctx, KK_CANONICAL_PROJECTED_POINT, 0);
         P(ctx, ";\n");
      }
   }
   if (!storage && vertex && !(mask & VARYING_BIT_POS))
      P_IND(ctx, "float4 position [[position]];\n");
   u_foreach_bit64(location, mask) {
      struct io_slot_info info = ctx->outputs_info[location];
      if (!info.num_components)
         continue;
      bool scalarized = VARYING_SLOT_INFO[location].scalarized;
      const char *type = location == VARYING_SLOT_CULL_PRIMITIVE ? "bool" :
                         location == VARYING_SLOT_LAYER || location == VARYING_SLOT_VIEWPORT ? "uint" :
                         alu_type_to_string(info.type);
      unsigned components = scalarized ? info.num_components : 1;
      const char *suffix = scalarized ? "" : vector_suffixes[info.num_components];
      for (unsigned c = 0; c < components; ++c) {
         P_IND(ctx, "%s%s%s ", storage && !scalarized && info.num_components == 3 ? "packed_" : "", type, suffix);
         varying_slot_name(ctx, location, c);
         P(ctx, " ");
         if (storage) {
            /* Staging carries only shader-visible values. Native position and
             * clip/cull attributes are materialized when copying outputs. */
         } else if (location == VARYING_SLOT_CULL_PRIMITIVE) {
            P(ctx, "[[primitive_culled]]");
         } else {
            varying_slot_semantic(ctx, location, c);
         }
         P(ctx, ";\n");
      }
   }
   if (!storage && vertex && ctx->shader->info.clip_distance_array_size)
      P_IND(ctx, "float gl_ClipDistance [[clip_distance]] [%u];\n",
            ctx->shader->info.clip_distance_array_size);
   if (!storage && !vertex && ctx->shader->info.cull_distance_array_size &&
       !(mask & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE)))
      P_IND(ctx, "bool kk_culled [[primitive_culled]];\n");
   if (!storage && !vertex && ctx->mesh.view_mask && !(mask & VARYING_BIT_LAYER))
      P_IND(ctx, "uint kk_mesh_view_layer [[render_target_array_index]];\n");
   if (!storage && !vertex &&
       !(mask & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE)) &&
       !ctx->shader->info.cull_distance_array_size) {
      P(ctx, "#ifdef KK_MESH_RETAINED\n");
      P_IND(ctx, "bool kk_retained_culled [[primitive_culled]];\n");
      P(ctx, "#endif\n");
   }
   ctx->indentlevel--;
   P(ctx, "};\n");
}

static void
kk_mesh_io_blocks(struct nir_to_msl_ctx *ctx)
{
   nir_shader *s = ctx->shader;
   uint64_t written = (s->info.outputs_written | s->info.outputs_read) &
      ~BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_INDICES) &
      ~BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_COUNT);
   ctx->mesh.retained_vertex_stride = kk_mesh_native_layout(ctx,
      kk_mesh_native_vertex_outputs(s, written & ~s->info.per_primitive_outputs), true);
   ctx->mesh.retained_primitive_stride = kk_mesh_native_layout(ctx,
      written & s->info.per_primitive_outputs, false);
   kk_mesh_io_block(ctx, "MeshVertex",
      kk_mesh_native_vertex_outputs(s, written & ~s->info.per_primitive_outputs), true, false);
   kk_mesh_io_block(ctx, "MeshPrimitive", written & s->info.per_primitive_outputs, false, false);
   kk_mesh_io_block(ctx, "MeshVertexStorage", written & ~s->info.per_primitive_outputs, true, true);
   kk_mesh_io_block(ctx, "MeshPrimitiveStorage", written & s->info.per_primitive_outputs, false, true);
   if (!ctx->mesh.retained_fields[2][0]) {
      ctx->mesh.retained_fields[2][0] = 4u;
      ctx->mesh.retained_fields[2][2] = nir_type_uint32;
      ctx->mesh.retained_fields[2][3] = 1u;
   }
   ctx->mesh.retained_fields[3][0] = ctx->mesh.view_mask ? 3u : 4u;
   ctx->mesh.retained_fields[3][1] = ctx->mesh.view_mask ? ctx->mesh.view_index_offset : 0u;
   ctx->mesh.retained_fields[3][2] = nir_type_uint32;
   ctx->mesh.retained_fields[3][3] = 1u;
   const char *topology = s->info.mesh.primitive_type == MESA_PRIM_POINTS ? "point" :
                          s->info.mesh.primitive_type == MESA_PRIM_LINES ? "line" : "triangle";
   P(ctx, "using MeshOutput = mesh<MeshVertex, MeshPrimitive, %u, %u, topology::%s>;\n",
     MAX2(1, s->info.mesh.max_vertices_out),
     MAX2(1, s->info.mesh.max_primitives_out), topology);
}

#endif
