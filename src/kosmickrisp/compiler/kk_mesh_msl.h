/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_MSL_H
#define KK_MESH_MSL_H
#include "kosmickrisp/libkk/kk_mesh_types.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#include "kk_mesh_canonical.h"

static void
kk_mesh_emit_buffers(struct nir_to_msl_ctx *ctx, bool attributes, bool params)
{
   P(ctx, "constant Buffer &buf0");
   if (attributes)
      P(ctx, " [[buffer(%u)]]", KK_ARGUMENT_ROOT);
   P(ctx, ", constant SamplerTable &sampler_table");
   if (attributes)
      P(ctx, " [[buffer(%u)]]", KK_ARGUMENT_SAMPLERS);
   if (ctx->uses_per_draw_data) {
      P(ctx, ", constant Buffer &per_draw");
      if (attributes)
         P(ctx, " [[buffer(%u)]]", KK_ARGUMENT_PER_DRAW);
   }
   if (!params) P(ctx, "\n#ifdef KK_MESH_RETAINED\n");
   {
      P(ctx, ", constant kk_mesh_params &params");
      if (attributes)
         P(ctx, " [[buffer(%u)]]", KK_ARGUMENT_MESH_PARAMETERS);
   }
   if (!params) P(ctx, "\n#endif\n");
}

static void
kk_mesh_emit_buffer_args(struct nir_to_msl_ctx *ctx)
{
   P(ctx, "buf0, sampler_table");
   if (ctx->uses_per_draw_data)
      P(ctx, ", per_draw");
   if (!ctx->mesh.queries) P(ctx, "\n#ifdef KK_MESH_RETAINED\n");
   P(ctx, ", params");
   if (!ctx->mesh.queries) P(ctx, "\n#endif\n");
}

static void
kk_mesh_emit_hardware_args(struct nir_to_msl_ctx *ctx)
{
   P(ctx, "uint3 kk_group [[threadgroup_position_in_grid]], "
          "uint3 kk_group_count [[threadgroups_per_grid]], "
          "uint3 kk_local [[thread_position_in_threadgroup]], "
          "uint kk_index [[thread_index_in_threadgroup]], "
          "uint kk_subgroup [[simdgroup_index_in_threadgroup]], "
          "uint kk_subgroups [[simdgroups_per_threadgroup]], "
          "uint kk_lane [[thread_index_in_simdgroup]], "
          "uint kk_width [[threads_per_simdgroup]], ");
}

static void
kk_mesh_emit_sysvals(struct nir_to_msl_ctx *ctx)
{
   P_IND(ctx, "kk_mesh_sysvals sys = {kk_group, kk_group_count, kk_local, "
              "kk_index, kk_subgroup, kk_subgroups, kk_lane, kk_width};\n");
}

static void
kk_mesh_emit_aliases(struct nir_to_msl_ctx *ctx)
{
   nir_shader *s = ctx->shader;
   P_IND(ctx, "uint3 gl_WorkGroupID = sys.group_id;\n");
   P_IND(ctx, "uint3 gl_NumWorkGroups = sys.group_count;\n");
   P_IND(ctx, "uint3 gl_LocalInvocationID = sys.local_id;\n");
   P_IND(ctx, "uint gl_LocalInvocationIndex = sys.local_index;\n");
   P_IND(ctx, "uint3 gl_GlobalInvocationID = sys.group_id * uint3(%u,%u,%u) + sys.local_id;\n",
         s->info.workgroup_size[0], s->info.workgroup_size[1], s->info.workgroup_size[2]);
   P_IND(ctx, "uint gl_SubGroupID = sys.subgroup_id;\n");
   P_IND(ctx, "uint gl_NumSubGroups = sys.subgroup_count;\n");
   P_IND(ctx, "uint gl_SubGroupInvocation = sys.subgroup_lane;\n");
   P_IND(ctx, "uint gl_SubGroupSize = sys.subgroup_size;\n");
}

static void
kk_mesh_emit_locals(struct nir_to_msl_ctx *ctx)
{
   if (ctx->shader->scratch_size)
      P_IND(ctx, "uchar scratch[%u] = {};\n", ctx->shader->scratch_size);
}

static void
kk_mesh_emit_shared(struct nir_to_msl_ctx *ctx, bool mesh)
{
   nir_shader *s = ctx->shader;
   P_IND(ctx, "threadgroup char shared_data[%u];\n", MAX2(1, s->info.shared_size));
   if (mesh) {
      unsigned threads = s->info.workgroup_size[0] * s->info.workgroup_size[1] * s->info.workgroup_size[2];
      /* One invocation needs no cross-thread output exchange. Keeping its
       * staging private avoids charging native output memory a second time
       * against Metal's combined mesh/threadgroup-memory limit. */
      const char *space = threads == 1 ? "thread" : "threadgroup";
      P_IND(ctx, "%s MeshVertexStorage kk_vertices[%u];\n", space, MAX2(1, s->info.mesh.max_vertices_out));
      P_IND(ctx, "%s MeshPrimitiveStorage kk_primitives[%u];\n", space, MAX2(1, s->info.mesh.max_primitives_out));
      P_IND(ctx, "%s uint kk_indices[%u];\n", space, MAX2(1,
         s->info.mesh.max_primitives_out * mesa_vertices_per_prim(s->info.mesh.primitive_type)));
      P_IND(ctx, "%s uint kk_counts[2];\n", space);
      P_IND(ctx, "if (kk_index == 0) { kk_counts[0] = 0; kk_counts[1] = 0; }\n");
      P_IND(ctx, "threadgroup_barrier(mem_flags::mem_threadgroup);\n");
   }
}

static void
kk_mesh_emit_copy_members(struct nir_to_msl_ctx *ctx, bool primitive)
{
   nir_shader *s = ctx->shader;
   uint64_t mask = (s->info.outputs_written | s->info.outputs_read) &
      ~BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_INDICES) &
      ~BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_COUNT);
   mask &= primitive ? s->info.per_primitive_outputs : ~s->info.per_primitive_outputs;
   if (ctx->mesh.flat_heap)
      mask &= ~((BITFIELD64_MASK(VARYING_SLOT_VAR31 + 1)) &
                ~BITFIELD64_MASK(VARYING_SLOT_VAR0));
   u_foreach_bit64(location, mask) {
      struct io_slot_info info = ctx->outputs_info[location];
      if (!info.num_components)
         continue;
      unsigned count = msl_output_num_components(ctx, location) == 1 ? info.num_components : 1;
      for (unsigned c = 0; c < count; ++c) {
         P_IND(ctx, "%s.", primitive ? "primitive" : "kk_vertex");
         msl_output_name(ctx, location, c, false);
         P(ctx, " = %s[i].", primitive ? "kk_primitives" : "kk_vertices");
         msl_output_name(ctx, location, c, false);
         P(ctx, ";\n");
      }
   }
}

static void
kk_mesh_emit_copy_outputs(struct nir_to_msl_ctx *ctx)
{
   nir_shader *s = ctx->shader;
   unsigned threads = s->info.workgroup_size[0] * s->info.workgroup_size[1] * s->info.workgroup_size[2];
   unsigned vpp = mesa_vertices_per_prim(s->info.mesh.primitive_type);
   P_IND(ctx, "threadgroup_barrier(mem_flags::mem_threadgroup);\n");
   P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
   P_IND(ctx, "device uchar *kk_cache = kk_mesh_retained_group(params, kk_group, kk_group_count);\n");
   P_IND(ctx, "if (kk_index == 0) { device kk_mesh_retained_count *c = (device kk_mesh_retained_count *)kk_cache; c->vertices = kk_counts[0]; c->primitives = kk_counts[1]; }\n");
   P(ctx, "#endif\n");
   P_IND(ctx, "for (uint i = kk_index; i < kk_counts[0]; i += %u) {\n", threads);
   ctx->indentlevel++;
   P_IND(ctx, "MeshVertex kk_vertex;\n");
   kk_mesh_emit_copy_members(ctx, false);
   if (!(s->info.outputs_written & VARYING_BIT_POS))
      P_IND(ctx, "kk_vertex.position = float4(0,0,0,1);\n");
   if (ctx->mesh.projected_basis) {
      P_IND(ctx, "kk_vertex.");
      msl_output_name(ctx, KK_CANONICAL_PROJECTED_POINT, 0, false);
      P(ctx, " = ");
      if (s->info.outputs_written & VARYING_BIT_POS) {
         P(ctx, "kk_vertices[i].");
         msl_output_name(ctx, VARYING_SLOT_POS, 0, false);
         P(ctx, ".xyw");
      } else {
         P(ctx, "float3(0, 0, 1)");
      }
      P(ctx, ";\n");
   }
   for (unsigned c = 0; c < s->info.clip_distance_array_size; ++c) {
      P_IND(ctx, "kk_vertex.gl_ClipDistance[%u] = kk_vertices[i].", c);
      msl_output_name(ctx, VARYING_SLOT_CLIP_DIST0 + c / 4, c % 4, false);
      P(ctx, ";\n");
   }
   P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
   P_IND(ctx, "*((device MeshVertex *)(kk_cache + KK_RETAINED_VERTICES + ulong(i) * KK_RETAINED_VERTEX_STRIDE)) = kk_vertex;\n");
   P(ctx, "#else\n");
   P_IND(ctx, "kk_output.set_vertex(i, kk_vertex);\n");
   P(ctx, "#endif\n");
   ctx->indentlevel--;
   P_IND(ctx, "}\n");
   P_IND(ctx, "for (uint i = kk_index; i < kk_counts[1]; i += %u) {\n", threads);
   ctx->indentlevel++;
   P_IND(ctx, "MeshPrimitive primitive = {};\n");
   if (ctx->mesh.flat_heap)
      kk_mesh_canonical_primitive(ctx);
   kk_mesh_emit_copy_members(ctx, true);
   if (ctx->mesh.view_mask) {
      P_IND(ctx, "primitive.");
      if (s->info.outputs_written & VARYING_BIT_LAYER)
         msl_output_name(ctx, VARYING_SLOT_LAYER, 0, false);
      else P(ctx, "kk_mesh_view_layer");
      P(ctx, " = *((constant uint *)((constant uchar *)&per_draw.contents[0] + %u));\n",
            ctx->mesh.view_index_offset);
   }
   if (s->info.cull_distance_array_size) {
      bool explicit_cull = s->info.per_primitive_outputs & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE);
      P_IND(ctx, "bool culled = ");
      if (explicit_cull) {
         P(ctx, "primitive.");
         msl_output_name(ctx, VARYING_SLOT_CULL_PRIMITIVE, 0, false);
      } else {
         P(ctx, "false");
      }
      P(ctx, ";\n");
      for (unsigned c = 0; c < s->info.cull_distance_array_size; ++c) {
         P_IND(ctx, "culled |= ");
         for (unsigned v = 0; v < vpp; ++v) {
            if (v)
               P(ctx, " && ");
            P(ctx, "(kk_vertices[kk_indices[i * %u + %u]].", vpp, v);
            msl_output_name(ctx, VARYING_SLOT_CULL_DIST0 + c / 4, c % 4, false);
            P(ctx, " < 0)");
         }
         P(ctx, ";\n");
      }
      P_IND(ctx, "primitive.");
      if (explicit_cull)
         msl_output_name(ctx, VARYING_SLOT_CULL_PRIMITIVE, 0, false);
      else
         P(ctx, "kk_culled");
      P(ctx, " = culled;\n");
   }
   P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
   P_IND(ctx, "*((device MeshPrimitive *)(kk_cache + KK_RETAINED_PRIMITIVES + ulong(i) * KK_RETAINED_PRIMITIVE_STRIDE)) = primitive;\n");
   P_IND(ctx, "((device uint *)(kk_cache + KK_RETAINED_LAYERS))[i] = ");
   if (ctx->mesh.view_mask || (s->info.outputs_written & VARYING_BIT_LAYER)) {
      P(ctx, "primitive.");
      if (s->info.outputs_written & VARYING_BIT_LAYER)
         msl_output_name(ctx, VARYING_SLOT_LAYER, 0, false);
      else P(ctx, "kk_mesh_view_layer");
   } else P(ctx, "0u");
   P(ctx, ";\n#else\n");
   P_IND(ctx, "kk_output.set_primitive(i, primitive);\n");
   P(ctx, "#endif\n");
   for (unsigned v = 0; v < vpp; ++v) {
      P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
      P_IND(ctx, "((device uint *)(kk_cache + KK_RETAINED_INDICES))[i * %u + %u] = kk_indices[i * %u + %u];\n", vpp, v, vpp, v);
      P(ctx, "#else\n");
      P_IND(ctx, "kk_output.set_index(i * %u + %u, kk_indices[i * %u + %u]);\n", vpp, v, vpp, v);
      P(ctx, "#endif\n");
   }
   ctx->indentlevel--;
   P_IND(ctx, "}\n");
   P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
   P_IND(ctx, "if (kk_index == 0) kk_output.set_primitive_count(0);\n");
   P(ctx, "#else\n");
   P_IND(ctx, "if (kk_index == 0) kk_output.set_primitive_count(kk_counts[1]);\n");
   P(ctx, "#endif\n");
}

#include "kk_mesh_retained_msl.h"

static void
kk_mesh_emit_functions(struct nir_to_msl_ctx *ctx, nir_function_impl *impl)
{
   nir_shader *s = ctx->shader;
   const char *name = get_entrypoint_name(s);
   bool task = s->info.stage == MESA_SHADER_TASK;
   unsigned threads = s->info.workgroup_size[0] * s->info.workgroup_size[1] * s->info.workgroup_size[2];
   if (!task) kk_mesh_emit_retained_reader(ctx);
   P(ctx, task ? "template<typename Payload, typename Emitter>\n" : "template<typename Payload>\n");
   P(ctx, "inline void %s_body(", name);
   if (!task)
      P(ctx, "MeshOutput kk_output, ");
   P(ctx, "Payload payload, ");
   if (task)
      P(ctx, "Emitter kk_emitter, ");
   P(ctx, "kk_mesh_sysvals sys, ");
   kk_mesh_emit_buffers(ctx, false, ctx->mesh.queries);
   P(ctx, ", threadgroup char *shared_data");
   if (!task) {
      const char *space = threads == 1 ? "thread" : "threadgroup";
      P(ctx, ", %s MeshVertexStorage *kk_vertices, %s MeshPrimitiveStorage *kk_primitives, %s uint *kk_indices, %s uint *kk_counts", space, space, space, space);
   }
   P(ctx, ") {\n");
   ctx->indentlevel++;
   kk_mesh_emit_aliases(ctx);
   kk_mesh_emit_locals(ctx);
   /* Function temporaries, such as sparse-access residency results, are
    * declared in the body as in the ordinary entry points. */
   msl_helper_locals(ctx, impl);
   if (ctx->mesh.queries)
      P_IND(ctx, "if (gl_LocalInvocationIndex == 0) kk_mesh_stat(params.%s_statistic, 1);\n", task ? "task" : "mesh");
   predeclare_ssa_values(ctx, impl);
   foreach_list_typed(nir_cf_node, node, node, &impl->body)
      cf_node_to_metal(ctx, node);
   ctx->indentlevel--;
   P(ctx, "}\n");

   /* Native object/mesh wrapper. The shader body is emitted only once. */
   P(ctx, "#ifndef KK_MESH_VIRTUAL_ONLY\n");
   P(ctx, "[[%s, max_total_threads_per_threadgroup(%u)]] void %s(", task ? "object" : "mesh", threads, name);
   if (task) {
      P(ctx, "object_data uchar *payload [[payload]], mesh_grid_properties kk_grid, ");
   } else {
      P(ctx, "MeshOutput kk_output, ");
      if (s->info.task_payload_size)
         P(ctx, "const object_data uchar *payload [[payload]], ");
   }
   kk_mesh_emit_hardware_args(ctx);
   kk_mesh_emit_buffers(ctx, true, ctx->mesh.queries);
   P(ctx, ") {\n");
   ctx->indentlevel++;
   kk_mesh_emit_sysvals(ctx);
   if (!task) {
      P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
      P_IND(ctx, "if (kk_index == 0) { device kk_mesh_retained_count *c = (device kk_mesh_retained_count *)kk_mesh_retained_group(params, kk_group, kk_group_count); c->vertices = 0; c->primitives = 0; }\n");
      P(ctx, "#endif\n");
   }
   kk_mesh_emit_shared(ctx, !task);
   P_IND(ctx, "%s_body(", name);
   if (!task)
      P(ctx, "kk_output, ");
   P(ctx, "%s, ", task || s->info.task_payload_size ? "payload" : "(const device uchar *)nullptr");
   if (task)
      P(ctx, "kk_grid, ");
   P(ctx, "sys, ");
   kk_mesh_emit_buffer_args(ctx);
   P(ctx, ", shared_data");
   if (!task)
      P(ctx, ", kk_vertices, kk_primitives, kk_indices, kk_counts");
   P(ctx, ");\n");
   if (!task)
      kk_mesh_emit_copy_outputs(ctx);
   ctx->indentlevel--;
   P(ctx, "}\n");
   P(ctx, "#endif\n");

   /* Compute task and mesh-only virtual wrappers share the exact same body. */
   P(ctx, "#ifdef KK_MESH_VIRTUAL\n");
   P(ctx, "[[%s, max_total_threads_per_threadgroup(%u)]] void %s_%s(", task ? "kernel" : "mesh", threads, name, task ? "compute" : "virtual");
   if (!task)
      P(ctx, "MeshOutput kk_output, ");
   kk_mesh_emit_hardware_args(ctx);
   kk_mesh_emit_buffers(ctx, true, true);
   P(ctx, ") {\n");
   ctx->indentlevel++;
   P_IND(ctx, "const device kk_mesh_batch *batch = (const device kk_mesh_batch *)params.batch;\n");
   P_IND(ctx, "device kk_mesh_task_record *records = (device kk_mesh_task_record *)params.records;\n");
   kk_mesh_emit_sysvals(ctx);
   if (!task) {
      P(ctx, "#ifdef KK_MESH_RETAINED_CAPTURE\n");
      P_IND(ctx, "if (kk_index == 0) { device kk_mesh_retained_count *c = (device kk_mesh_retained_count *)kk_mesh_retained_group(params, kk_group, kk_group_count); c->vertices = 0; c->primitives = 0; }\n");
      P(ctx, "#endif\n");
   }
   if (task) {
      P_IND(ctx, "uint parent = kk_group.x;\n");
      P_IND(ctx, "sys.group_id = kk_mesh_unflatten(params.task_base + parent, uint3(batch->input_grid[0],batch->input_grid[1],batch->input_grid[2]));\n");
      P_IND(ctx, "sys.group_count = uint3(batch->input_grid[0],batch->input_grid[1],batch->input_grid[2]);\n");
   } else {
      P_IND(ctx, "uint index = kk_group.x + kk_group.y * batch->mesh_dispatch[0];\n");
      P_IND(ctx, "if (index >= batch->mesh_count) { if (kk_index == 0) kk_output.set_primitive_count(0); return; }\n");
      P_IND(ctx, "uint parent = kk_mesh_parent(index, records, batch->task_count);\n");
      P_IND(ctx, "uint local = index - (parent ? records[parent-1].prefix_end : 0);\n");
      P_IND(ctx, "sys.group_count = uint3(records[parent].grid[0],records[parent].grid[1],records[parent].grid[2]);\n");
      P_IND(ctx, "sys.group_id = kk_mesh_unflatten(local, sys.group_count);\n");
   }
   P_IND(ctx, "%sdevice uchar *payload = (%sdevice uchar *)params.payloads + ulong(parent) * params.payload_stride;\n", task ? "" : "const ", task ? "" : "const ");
   kk_mesh_emit_shared(ctx, !task);
   P_IND(ctx, "%s_body(", name);
   if (!task)
      P(ctx, "kk_output, ");
   P(ctx, "payload, ");
   if (task)
      P(ctx, "records + parent, ");
   P(ctx, "sys, ");
   kk_mesh_emit_buffer_args(ctx);
   P(ctx, ", shared_data");
   if (!task)
      P(ctx, ", kk_vertices, kk_primitives, kk_indices, kk_counts");
   P(ctx, ");\n");
   if (!task)
      kk_mesh_emit_copy_outputs(ctx);
   ctx->indentlevel--;
   P(ctx, "}\n");
   P(ctx, "#endif\n");
}

#endif
