/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_CANONICAL_EMITTER_H
#define KK_MESH_CANONICAL_EMITTER_H

#include "poly/geometry.h"
#include "kosmickrisp/libkk/kk_canonical_layout.h"

/* This is another producer of the existing canonical packet. Allocation uses
 * the existing poly_heap base/bottom/size, including its alignment policy. */
static void
kk_mesh_canonical_primitive(struct nir_to_msl_ctx *ctx)
{
   const unsigned size = ctx->mesh.projected_basis ?
      kk_canonical_projected_buffer_size() : kk_canonical_buffer_size();
   P_IND(ctx, "ulong kk_heap = *((constant ulong *)((constant uchar *)&per_draw.contents[0] + %u));\n",
         ctx->mesh.flat_heap_offset);
   P_IND(ctx, "uint kk_bottom = atomic_fetch_add_explicit((device atomic_uint *)(kk_heap + %zu), %uu, memory_order_relaxed);\n",
         offsetof(struct poly_heap, bottom), size + 16u);
   P_IND(ctx, "uint kk_offset = (kk_bottom + 15u) & ~15u;\n");
   P_IND(ctx, "uint kk_heap_size = *((device const uint *)(kk_heap + %zu));\n",
         offsetof(struct poly_heap, size));
   P_IND(ctx, "if (kk_offset + %uu > kk_heap_size) kk_offset = 0u;\n", size);
   P_IND(ctx, "ulong kk_packet = *((device const ulong *)(kk_heap + %zu)) + kk_offset;\n",
         offsetof(struct poly_heap, base));

   uint64_t generic = ctx->shader->info.outputs_written &
      BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1) &
      ~BITFIELD64_MASK(VARYING_SLOT_VAR0);
   unsigned vpp = mesa_vertices_per_prim(ctx->shader->info.mesh.primitive_type);
   for (unsigned corner = 0; corner < 3; ++corner) {
      if (ctx->mesh.projected_basis) {
         for (unsigned c = 0; c < 3; ++c) {
            P_IND(ctx, "*((device uint *)(kk_packet + %uu)) = as_type<uint>(",
                  kk_canonical_position_offset(corner) + c * 4u);
            if (ctx->shader->info.outputs_written & VARYING_BIT_POS) {
               P(ctx, "kk_vertices[kk_indices[i * %u + %u]].", vpp,
                 MIN2(corner, vpp - 1));
               msl_output_name(ctx, VARYING_SLOT_POS, 0, false);
               P(ctx, ".%c", "xyw"[c]);
            } else {
               P(ctx, "%s", c == 2 ? "1.0f" : "0.0f");
            }
            P(ctx, ");\n");
         }
      }
      u_foreach_bit64(location, generic) {
         struct io_slot_info info = ctx->outputs_info[location];
         bool primitive = ctx->shader->info.per_primitive_outputs &
                          BITFIELD64_BIT(location);
         for (unsigned c = 0; c < info.num_components; ++c) {
            unsigned bits = nir_alu_type_get_type_size(info.type);
            P_IND(ctx, "*((device uint *)(kk_packet + %uu)) = ",
                  kk_canonical_buffer_offset(location, corner) + c * 4u);
            P(ctx, bits == 16 ? "uint(as_type<ushort>(" : "as_type<uint>(");
            if (primitive) {
               P(ctx, "kk_primitives[i].");
            } else {
               P(ctx, "kk_vertices[kk_indices[i * %u + %u]].", vpp,
                 MIN2(corner, vpp - 1));
            }
            msl_output_name(ctx, location, 0, false);
            if (info.num_components > 1)
               P(ctx, ".%c", "xyzw"[c]);
            P(ctx, bits == 16 ? "));\n" : ");\n");
         }
      }
   }
   P_IND(ctx, "primitive.");
   msl_output_name(ctx, VARYING_SLOT_VAR0, 0, false);
   P(ctx, " = as_type<uint2>(kk_packet);\n");
}

#endif
