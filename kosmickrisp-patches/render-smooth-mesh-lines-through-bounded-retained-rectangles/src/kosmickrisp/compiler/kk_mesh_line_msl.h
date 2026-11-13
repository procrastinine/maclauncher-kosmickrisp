/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_LINE_MSL_H
#define KK_MESH_LINE_MSL_H

/* A reader group expands at most64 original lines:256 vertices/128 triangles.
 * Capture executes the application once. Additional readers consume its
 * immutable records; neither Task nor Mesh side effects are repeated. */
static void
kk_mesh_emit_line_reader(struct nir_to_msl_ctx *ctx, unsigned stride)
{
   nir_shader *s = ctx->shader;
   P(ctx, "#ifdef KK_MESH_LINE_READER\n");
   msl_mesh_line_primitive(ctx);
   P(ctx, "using LineOutput = mesh<MeshVertex, LinePrimitive, 256, 128, topology::triangle>;\n");
   P(ctx, "struct kk_mesh_line_state { float4 viewport[16]; float width; };\n");
   P(ctx, "[[mesh, max_total_threads_per_threadgroup(1)]] void %s_line(LineOutput output, uint3 group [[threadgroup_position_in_grid]], uint3 grid [[threadgroups_per_grid]], constant kk_mesh_params &params [[buffer(%u)]], constant uint2 *selector [[buffer(%u)]]) {\n",
     get_entrypoint_name(s), KK_ARGUMENT_MESH_PARAMETERS, KK_ARGUMENT_POLY_LAYER_REPLAY);
   P(ctx, "device const uchar *cache = (device const uchar *)params.indirect_count + ulong(group.x + grid.x * (group.y + grid.y * group.z)) * %uu;\n", stride);
   P(ctx, "device const kk_mesh_retained_count *count = (device const kk_mesh_retained_count *)cache;\n");
   P(ctx, "if (count->vertices > %uu || count->primitives > %uu || params.task_base >= count->primitives) { output.set_primitive_count(0); return; }\n",
     s->info.mesh.max_vertices_out, s->info.mesh.max_primitives_out);
   P(ctx, "device const kk_mesh_line_state &state = *((device const kk_mesh_line_state *)params.payloads);\n");
   P(ctx, "uint lines = min(64u, count->primitives - params.task_base);\nfor (uint i = 0; i < lines; ++i) {\nuint slot = params.task_base + i;\n");
   P(ctx, "device const uint *indices = (device const uint *)(cache + KK_RETAINED_INDICES);\nuint ia = indices[slot * 2], ib = indices[slot * 2 + 1];\n");
   P(ctx, "if (ia >= count->vertices || ib >= count->vertices) { LinePrimitive p = {}; p.");
   if (s->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE))
      msl_output_name(ctx, VARYING_SLOT_CULL_PRIMITIVE, 0, false);
   else P(ctx, s->info.cull_distance_array_size ? "kk_culled" : "kk_retained_culled");
   P(ctx, " = true; output.set_primitive(i*2,p); output.set_primitive(i*2+1,p); continue; }\n");
   P(ctx, "MeshVertex a = *((device const MeshVertex *)(cache + KK_RETAINED_VERTICES + ulong(ia) * KK_RETAINED_VERTEX_STRIDE));\n");
   P(ctx, "MeshVertex b = *((device const MeshVertex *)(cache + KK_RETAINED_VERTICES + ulong(ib) * KK_RETAINED_VERTEX_STRIDE));\n");
   P(ctx, "MeshPrimitive source = *((device const MeshPrimitive *)(cache + KK_RETAINED_PRIMITIVES + ulong(slot) * KK_RETAINED_PRIMITIVE_STRIDE));\nLinePrimitive primitive = {};\n");
   msl_mesh_line_copy_primitive(ctx);
   P(ctx, "if (selector && selector->y) { primitive.");
   if (s->info.per_primitive_outputs & BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE)) msl_output_name(ctx,VARYING_SLOT_CULL_PRIMITIVE,0,false);
   else P(ctx,s->info.cull_distance_array_size ? "kk_culled" : "kk_retained_culled");
   P(ctx," |= ((device const uint *)(cache + KK_RETAINED_LAYERS))[slot] != params.predicate_inverted;\n");
   if (ctx->mesh.view_mask || (s->info.per_primitive_outputs & VARYING_BIT_LAYER)) {
      P(ctx,"primitive.");
      if (s->info.per_primitive_outputs & VARYING_BIT_LAYER) msl_output_name(ctx,VARYING_SLOT_LAYER,0,false);
      else P(ctx,"kk_mesh_view_layer");
      P(ctx," = 0u;\n");
   }
   P(ctx, "}\nuint viewport_index = ");
   if (s->info.outputs_written & VARYING_BIT_VIEWPORT) {
      P(ctx,(s->info.per_primitive_outputs & VARYING_BIT_VIEWPORT) ? "source." : "a.");
      msl_output_name(ctx,VARYING_SLOT_VIEWPORT,0,false);
   } else P(ctx,"0u");
   P(ctx,";\nfloat4 vp = state.viewport[min(viewport_index,15u)];\n");
   P(ctx,"float2 sa = (a.position.xy / a.position.w * 0.5f + 0.5f) * vp.zw + vp.xy;\nfloat2 sb = (b.position.xy / b.position.w * 0.5f + 0.5f) * vp.zw + vp.xy;\n");
   P(ctx,"float2 delta = sb - sa; float len = length(delta); float2 tangent = delta / max(len,1.0e-15f); float2 normal = float2(-tangent.y,tangent.x);\n");
   P(ctx,"primitive.kk_line_endpoints = float4(sa,sb); primitive.kk_line_width = state.width;\n");
   P(ctx,"for (uint corner = 0; corner < 4; ++corner) { bool last = (corner & 2u) != 0; MeshVertex vtx = last ? b : a;\n");
   P(ctx,"float2 offset = normal * ((corner & 1u) ? 0.5f : -0.5f) * (state.width + 1.0f) + tangent * (last ? 0.5f : -0.5f);\n");
   P(ctx,"vtx.position.xy += offset * 2.0f / vp.zw * vtx.position.w; output.set_vertex(i*4+corner,vtx); }\n");
   P(ctx,"output.set_primitive(i*2,primitive); output.set_primitive(i*2+1,primitive);\nuint base = i*4; const uint corners[6] = {0,2,3,0,3,1}; for (uint c = 0; c < 6; ++c) output.set_index(i*6+c,base+corners[c]);\n}\noutput.set_primitive_count(lines*2);\n}\n#endif\n");
}
#endif
