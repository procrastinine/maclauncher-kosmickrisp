/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_POINT_MSL_H
#define KK_MESH_POINT_MSL_H

/* VK_POLYGON_MODE_POINT for a triangle Mesh shader. A reader group reads at
 * most 64 captured triangles and draws each triangle's own vertices that pass
 * the point clip test as points: 192 vertices and primitives at most. Culling
 * uses the triangle's facing before the points are drawn. Capture executes the
 * application once; neither Task nor Mesh side effects are repeated. */
static void
kk_mesh_emit_point_reader(struct nir_to_msl_ctx *ctx, unsigned stride)
{
   nir_shader *s = ctx->shader;
   const char *culled = (s->info.per_primitive_outputs & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE))
      ? NULL : s->info.cull_distance_array_size ? "kk_culled" : "kk_retained_culled";
   P(ctx, "#ifdef KK_MESH_POINT_READER\n");
   msl_mesh_point_vertex(ctx);
   P(ctx, "using PointOutput = mesh<PointVertex, MeshPrimitive, 192, 192, topology::point>;\n");
   P(ctx, "struct kk_mesh_point_state { float4 viewport[16]; uint cull_mode; uint front_ccw; uint depth_clip; uint pad; };\n");
   P(ctx, "[[mesh, max_total_threads_per_threadgroup(1)]] void %s_point(PointOutput output, uint3 group [[threadgroup_position_in_grid]], uint3 grid [[threadgroups_per_grid]], constant kk_mesh_params &params [[buffer(%u)]], constant uint2 *selector [[buffer(%u)]]) {\n",
     get_entrypoint_name(s), KK_ARGUMENT_MESH_PARAMETERS, KK_ARGUMENT_POLY_LAYER_REPLAY);
   P(ctx, "device const uchar *cache = (device const uchar *)params.indirect_count + ulong(group.x + grid.x * (group.y + grid.y * group.z)) * %uu;\n", stride);
   P(ctx, "device const kk_mesh_retained_count *count = (device const kk_mesh_retained_count *)cache;\n");
   P(ctx, "if (count->vertices > %uu || count->primitives > %uu || params.task_base >= count->primitives) { output.set_primitive_count(0); return; }\n",
     s->info.mesh.max_vertices_out, s->info.mesh.max_primitives_out);
   P(ctx, "device const kk_mesh_point_state &state = *((device const kk_mesh_point_state *)params.payloads);\n");
   P(ctx, "device const uint *indices = (device const uint *)(cache + KK_RETAINED_INDICES);\n");
   P(ctx, "uint triangles = min(64u, count->primitives - params.task_base), points = 0;\n");
   P(ctx, "for (uint i = 0; i < triangles; ++i) {\nuint slot = params.task_base + i;\n");
   P(ctx, "uint index[3] = {indices[slot * 3], indices[slot * 3 + 1], indices[slot * 3 + 2]};\n");
   P(ctx, "if (index[0] >= count->vertices || index[1] >= count->vertices || index[2] >= count->vertices) continue;\n");
   P(ctx, "MeshPrimitive primitive = *((device const MeshPrimitive *)(cache + KK_RETAINED_PRIMITIVES + ulong(slot) * KK_RETAINED_PRIMITIVE_STRIDE));\n");
   P(ctx, "if (primitive.");
   if (culled) {
      P(ctx, "%s", culled);
   } else {
      msl_output_name(ctx, VARYING_SLOT_CULL_PRIMITIVE, 0, false);
   }
   P(ctx, ") continue;\n");
   P(ctx, "if (selector && selector->y) {\nif (((device const uint *)(cache + KK_RETAINED_LAYERS))[slot] != params.predicate_inverted) continue;\n");
   if (ctx->mesh.view_mask || (s->info.per_primitive_outputs & VARYING_BIT_LAYER)) {
      P(ctx, "primitive.");
      if (s->info.per_primitive_outputs & VARYING_BIT_LAYER) msl_output_name(ctx, VARYING_SLOT_LAYER, 0, false);
      else P(ctx, "kk_mesh_view_layer");
      P(ctx, " = 0u;\n");
   }
   P(ctx, "}\nMeshVertex v[3];\nfor (uint k = 0; k < 3; ++k) v[k] = *((device const MeshVertex *)(cache + KK_RETAINED_VERTICES + ulong(index[k]) * KK_RETAINED_VERTEX_STRIDE));\n");
   P(ctx, "uint viewport_index = ");
   if (s->info.outputs_written & VARYING_BIT_VIEWPORT) {
      P(ctx, (s->info.per_primitive_outputs & VARYING_BIT_VIEWPORT) ? "primitive." : "v[0].");
      msl_output_name(ctx, VARYING_SLOT_VIEWPORT, 0, false);
   } else {
      P(ctx, "0u");
   }
   /* Facing of the homogeneous triangle: its sign holds for the visible part
    * when vertices lie behind the eye. Vulkan's framebuffer area carries a
    * leading minus sign; a zero area is back-facing for either front face. */
   P(ctx, ";\nfloat4 vp = state.viewport[min(viewport_index, 15u)];\n");
   P(ctx, "float area = -determinant(float3x3(v[0].position.xyw, v[1].position.xyw, v[2].position.xyw)) * vp.z * vp.w;\n");
   P(ctx, "bool front = state.front_ccw ? area > 0.0f : area < 0.0f;\n");
   P(ctx, "if ((front && (state.cull_mode & 1u)) || (!front && (state.cull_mode & 2u))) continue;\n");
   P(ctx, "for (uint k = 0; k < 3; ++k) {\nfloat4 p = v[k].position;\n");
   P(ctx, "bool outside = any(float4(p.w - p.x, p.w + p.x, p.w - p.y, p.w + p.y) < 0.0f) || (state.depth_clip && (p.z < 0.0f || p.w - p.z < 0.0f));\n");
   for (unsigned c = 0; c < s->info.clip_distance_array_size; ++c)
      P(ctx, "outside |= v[k].gl_ClipDistance[%u] < 0.0f;\n", c);
   P(ctx, "if (outside) continue;\n");
   P(ctx, "output.set_vertex(points, kk_mesh_point_vertex(v[k]));\noutput.set_primitive(points, primitive);\noutput.set_index(points, points);\n++points;\n}\n}\n");
   P(ctx, "output.set_primitive_count(points);\n}\n#endif\n");
}
#endif
