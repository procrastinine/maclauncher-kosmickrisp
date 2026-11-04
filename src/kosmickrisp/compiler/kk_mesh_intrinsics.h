/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_INTRINSICS_H
#define KK_MESH_INTRINSICS_H

static void
kk_mesh_scalar_src(struct nir_to_msl_ctx *ctx, nir_src *src, unsigned component)
{
   src_to_msl(ctx, src);
   if (src->ssa->num_components > 1)
      P(ctx, ".%c", "xyzw"[component]);
}

static void
kk_mesh_io_member(struct nir_to_msl_ctx *ctx, nir_src *index,
                   unsigned location, unsigned component, bool primitive)
{
   P(ctx, "%s[", primitive ? "kk_primitives" : "kk_vertices");
   src_to_msl(ctx, index);
   P(ctx, "].");
   msl_output_name(ctx, location, component, false);
   if (msl_output_num_components(ctx, location) > 1)
      P(ctx, ".%c", "xyzw"[component]);
}

static void
kk_mesh_io_slot(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *intr,
                 nir_src *index, unsigned location, bool primitive, bool store)
{
   unsigned component = nir_intrinsic_component(intr);
   unsigned mask = store ? nir_intrinsic_write_mask(intr) :
                            BITFIELD_MASK(intr->num_components);
   bool indices = location == VARYING_SLOT_PRIMITIVE_INDICES;
   unsigned vpp = mesa_vertices_per_prim(ctx->shader->info.mesh.primitive_type);
   if (!store)
      P(ctx, "%s(", msl_type_for_def(ctx->types, &intr->def));
   for (unsigned c = 0; c < intr->num_components; ++c) {
      if (!(mask & BITFIELD_BIT(c)))
         continue;
      if (store)
         P_INDENT(ctx);
      else if (c)
         P(ctx, ", ");
      if (indices) {
         P(ctx, "kk_indices[(");
         src_to_msl(ctx, index);
         P(ctx, ") * %u + %u]", vpp, component + c);
      } else {
         kk_mesh_io_member(ctx, index, location, component + c, primitive);
      }
      if (store) {
         P(ctx, " = ");
         kk_mesh_scalar_src(ctx, &intr->src[0], c);
         P(ctx, ";\n");
      }
   }
   if (!store)
      P(ctx, ")");
}

static void
kk_mesh_intrinsic_to_msl(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *intr)
{
   assert(ctx->shader->info.stage == MESA_SHADER_TASK ||
          ctx->shader->info.stage == MESA_SHADER_MESH);
   bool store, primitive;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_task_payload: {
      unsigned bits = intr->def.bit_size;
      const char *type = msl_uint_type(bits, 1);
      P(ctx, "%s(", msl_type_for_def(ctx->types, &intr->def));
      for (unsigned c = 0; c < intr->num_components; ++c) {
         if (c)
            P(ctx, ", ");
         P(ctx, "kk_payload_load<%s>(payload, uint(", type);
         src_to_msl(ctx, &intr->src[0]);
         P(ctx, ") + %u)", nir_intrinsic_base(intr) + c * bits / 8);
      }
      P(ctx, ");\n");
      return;
   }
   case nir_intrinsic_store_task_payload: {
      unsigned bits = intr->src[0].ssa->bit_size;
      const char *type = msl_uint_type(bits, 1);
      for (unsigned c = 0; c < intr->num_components; ++c) {
         if (!(nir_intrinsic_write_mask(intr) & BITFIELD_BIT(c)))
            continue;
         P_IND(ctx, "kk_payload_store<%s>(payload, uint(", type);
         src_to_msl(ctx, &intr->src[1]);
         P(ctx, ") + %u, ", nir_intrinsic_base(intr) + c * bits / 8);
         kk_mesh_scalar_src(ctx, &intr->src[0], c);
         P(ctx, ");\n");
      }
      return;
   }
   case nir_intrinsic_launch_mesh_workgroups:
      /* EmitMeshTasksEXT synchronizes the entire task workgroup and makes
       * payload/shared writes visible before any mesh stage can consume it. */
      P_IND(ctx, "kk_payload_barrier(payload, mem_flags::mem_threadgroup);\n");
      P_IND(ctx, "if (gl_LocalInvocationIndex == 0) kk_emit_mesh(kk_emitter, ");
      src_to_msl(ctx, &intr->src[0]);
      P(ctx, ");\n");
      return;
   case nir_intrinsic_set_vertex_and_primitive_count:
      P_IND(ctx, "if (gl_LocalInvocationIndex == 0) { kk_counts[0] = ");
      src_to_msl(ctx, &intr->src[0]);
      P(ctx, "; kk_counts[1] = ");
      src_to_msl(ctx, &intr->src[1]);
      P(ctx, ";");
      if (ctx->mesh.queries)
         P(ctx, " kk_mesh_stat(params.primitive_statistic, kk_counts[1]);");
      P(ctx, " }\n");
      return;
   case nir_intrinsic_store_per_vertex_output:
      store = true; primitive = false; break;
   case nir_intrinsic_store_per_primitive_output:
      store = true; primitive = true; break;
   case nir_intrinsic_load_per_vertex_output:
      store = false; primitive = false; break;
   case nir_intrinsic_load_per_primitive_output:
      store = false; primitive = true; break;
   default:
      UNREACHABLE("Invalid Mesh intrinsic");
   }
   nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   nir_src *index = &intr->src[store ? 1 : 0];
   nir_src *offset = &intr->src[store ? 2 : 1];
   if (nir_src_is_const(*offset)) {
      kk_mesh_io_slot(ctx, intr, index, io.location + nir_src_as_uint(*offset),
                       primitive, store);
      if (!store)
         P(ctx, ";\n");
   } else if (!store) {
      /* Loads are expressions in the ordinary intrinsic dispatcher. Select
       * the same declared slot without introducing an extra SSA assignment. */
      for (unsigned i = 0; i < io.num_slots; ++i) {
         src_to_msl(ctx, offset);
         P(ctx, " == %uu ? ", i);
         kk_mesh_io_slot(ctx, intr, index, io.location + i, primitive, false);
         P(ctx, " : ");
      }
      /* An out-of-range output read is undefined. */
      P(ctx, "%s(0);\n", msl_type_for_def(ctx->types, &intr->def));
   } else {
      P_IND(ctx, "switch (");
      src_to_msl(ctx, offset);
      P(ctx, ") {\n");
      ctx->indentlevel++;
      for (unsigned i = 0; i < io.num_slots; ++i) {
         P_IND(ctx, "case %u: {\n", i);
         ctx->indentlevel++;
         kk_mesh_io_slot(ctx, intr, index, io.location + i, primitive, true);
         P_IND(ctx, "break;\n");
         ctx->indentlevel--;
         P_IND(ctx, "}\n");
      }
      ctx->indentlevel--;
      P_IND(ctx, "}\n");
   }
}

#endif
