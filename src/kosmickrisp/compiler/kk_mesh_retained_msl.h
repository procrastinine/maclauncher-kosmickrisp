/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_RETAINED_MSL_H
#define KK_MESH_RETAINED_MSL_H
#include "kk_mesh_line_msl.h"
#include "kk_mesh_point_msl.h"
/* Native final output records preserve position, viewport, clip/cull and
 * primitive-ID members as well as the canonical packet pointer. */
static void
kk_mesh_emit_retained_reader(struct nir_to_msl_ctx *ctx)
{
   nir_shader *s = ctx->shader;
   unsigned v = MAX2(s->info.mesh.max_vertices_out, 1u);
   unsigned p = MAX2(s->info.mesh.max_primitives_out, 1u);
   unsigned vpp = mesa_vertices_per_prim(s->info.mesh.primitive_type);
   unsigned vs = ctx->mesh.retained_vertex_stride;
   unsigned ps = ctx->mesh.retained_primitive_stride;
   /* Every native field alignment divides32. Section starts stay aligned even
    * when the preceding array has an odd number of small primitive records. */
   unsigned vertices = 32u;
   unsigned primitives = ALIGN_POT(vertices + v * vs, 32u);
   unsigned indices = ALIGN_POT(primitives + p * ps, 4u);
   unsigned layers = indices + p * vpp * 4u;
   unsigned stride = ALIGN_POT(layers + p * 4u, 32u);
   P(ctx, "#ifdef KK_MESH_RETAINED\n");
   P(ctx, "constant uint KK_RETAINED_VERTICES = %uu;\n", vertices);
   P(ctx, "constant uint KK_RETAINED_VERTEX_STRIDE = %uu;\n", vs);
   P(ctx, "constant uint KK_RETAINED_PRIMITIVE_STRIDE = %uu;\n", ps);
   P(ctx, "constant uint KK_RETAINED_PRIMITIVES = %uu;\n", primitives);
   P(ctx, "constant uint KK_RETAINED_INDICES = %uu;\n", indices);
   P(ctx, "constant uint KK_RETAINED_LAYERS = %uu;\n", layers);
   P(ctx, "static_assert(sizeof(MeshVertex) == KK_RETAINED_VERTEX_STRIDE);\n");
   P(ctx, "static_assert(sizeof(MeshPrimitive) == KK_RETAINED_PRIMITIVE_STRIDE);\n");
   P(ctx, "inline device uchar *kk_mesh_retained_group(constant kk_mesh_params &params, uint3 group, uint3 grid) { return (device uchar *)params.indirect_count + ulong(group.x + grid.x * (group.y + grid.y * group.z)) * %uu; }\n", stride);
   P(ctx, "#endif\n#ifdef KK_MESH_RETAINED_READER\n");
   P(ctx, "[[mesh, max_total_threads_per_threadgroup(1)]] void %s_retained(MeshOutput kk_output, uint3 kk_group [[threadgroup_position_in_grid]], uint3 kk_group_count [[threadgroups_per_grid]], constant kk_mesh_params &params [[buffer(%u)]], constant uint2 *poly_layer_selector [[buffer(%u)]]) {\n", get_entrypoint_name(s), KK_ARGUMENT_MESH_PARAMETERS, KK_ARGUMENT_POLY_LAYER_REPLAY);
   P(ctx, "uint original_slot = ~0u;\n");
   P(ctx, "device uchar *cache = kk_mesh_retained_group(params, kk_group, kk_group_count);\n");
   P(ctx, "if (params.records) {\n");
   P(ctx, "device const kk_mesh_retained_job *job = (device const kk_mesh_retained_job *)params.records;\n");
   P(ctx, "if (!params.indirect_grid) { kk_output.set_primitive_count(0); return; }\n");
   P(ctx, "device const uint *grid = (device const uint *)params.indirect_grid;\n");
   P(ctx, "uint group = job->physical_group;\n");
   P(ctx, "if (!kk_mesh_retained_job_group_valid(group, grid[0], grid[1], grid[2], params.max_draw_count)) { kk_output.set_primitive_count(0); return; }\n");
   P(ctx, "cache = (device uchar *)params.indirect_count + ulong(group) * %uu;\n", stride);
   P(ctx, "device const kk_mesh_retained_count *valid_count = (device const kk_mesh_retained_count *)cache;\n");
   P(ctx, "original_slot = job->original_primitive_slot;\n");
   P(ctx, "if (!kk_mesh_retained_job_slot_valid(original_slot, valid_count->vertices, valid_count->primitives, %uu, %uu)) { kk_output.set_primitive_count(0); return; }\n}\n", s->info.mesh.max_vertices_out, s->info.mesh.max_primitives_out);
   P(ctx, "device const kk_mesh_retained_count *count = (device const kk_mesh_retained_count *)cache;\n");
   P(ctx, "for (uint i = 0; i < count->vertices; ++i) kk_output.set_vertex(i, *((device const MeshVertex *)(cache + KK_RETAINED_VERTICES + ulong(i) * KK_RETAINED_VERTEX_STRIDE)));\n");
   P(ctx, "for (uint i = 0; i < count->primitives; ++i) {\n");
   P(ctx, "MeshPrimitive primitive = *((device const MeshPrimitive *)(cache + KK_RETAINED_PRIMITIVES + ulong(i) * KK_RETAINED_PRIMITIVE_STRIDE));\n");
   P(ctx, "primitive.");
   if (s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE))
      msl_output_name(ctx, VARYING_SLOT_CULL_PRIMITIVE, 0, false);
   else P(ctx, s->info.cull_distance_array_size ? "kk_culled" : "kk_retained_culled");
   P(ctx, " |= (params.records && i != original_slot);\n");
   P(ctx, "if (poly_layer_selector && poly_layer_selector->y) {\nprimitive.");
   if (s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE))
      msl_output_name(ctx, VARYING_SLOT_CULL_PRIMITIVE, 0, false);
   else P(ctx, s->info.cull_distance_array_size ? "kk_culled" : "kk_retained_culled");
   P(ctx, " |= ((device const uint *)(cache + KK_RETAINED_LAYERS))[i] != params.predicate_inverted;\n");
   if (ctx->mesh.view_mask || (s->info.outputs_written & VARYING_BIT_LAYER)) {
      P(ctx, "primitive.");
      if (s->info.outputs_written & VARYING_BIT_LAYER) msl_output_name(ctx, VARYING_SLOT_LAYER, 0, false);
      else P(ctx, "kk_mesh_view_layer");
      P(ctx, " = 0u;\n");
   }
   P(ctx, "}\nkk_output.set_primitive(i, primitive);\n");
   P(ctx, "for (uint j = 0; j < %uu; ++j) kk_output.set_index(i * %uu + j, ((device const uint *)(cache + KK_RETAINED_INDICES))[i * %uu + j]);\n", vpp, vpp, vpp);
   P(ctx, "}\nkk_output.set_primitive_count(count->primitives);\n}\n#endif\n");
   if (vpp == 2) kk_mesh_emit_line_reader(ctx, stride);
   if (vpp == 3 && ctx->mesh.point_reader) kk_mesh_emit_point_reader(ctx, stride);
}
#endif
