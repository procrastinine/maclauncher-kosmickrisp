// SPDX-License-Identifier: MIT
export function apply({edit}) {
// A passthrough GS has the reduced output class. Choose adjacency vertices
  // from the draw's topology, rather than the pipeline's initial dynamic value.
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    'key->prim = u_decomposed_prim(vk_topology_to_mesa(state->ia->primitive_topology));',
    'key->prim = u_reduced_prim(vk_topology_to_mesa(state->ia->primitive_topology));'));
  edit('src/poly/nir/poly_nir_lower_gs.c', s => s
    .replace(`   case MESA_PRIM_LINES:
      return poly_vertex_id_for_line_class(b, topology, prim, vert, nr);`,
      `   case MESA_PRIM_LINES: {
      nir_def *adj = nir_ior(b,
         nir_ieq_imm(b, topology, MESA_PRIM_LINES_ADJACENCY),
         nir_ieq_imm(b, topology, MESA_PRIM_LINE_STRIP_ADJACENCY));
      return nir_bcsel(b, adj,
         poly_vertex_id_for_line_adj_class(b, topology, prim,
                                           nir_iadd_imm(b, vert, 1)),
         poly_vertex_id_for_line_class(b, topology, prim, vert, nr));
   }`)
    .replace(`   case MESA_PRIM_TRIANGLES:
      return poly_vertex_id_for_tri_class(b, topology, prim, vert,
                                          flatshade_first);`,
      `   case MESA_PRIM_TRIANGLES: {
      nir_def *adj = nir_ior(b,
         nir_ieq_imm(b, topology, MESA_PRIM_TRIANGLES_ADJACENCY),
         nir_ieq_imm(b, topology, MESA_PRIM_TRIANGLE_STRIP_ADJACENCY));
      return nir_bcsel(b, adj,
         poly_vertex_id_for_tri_adj_class(b, topology, prim,
            nir_imul_imm(b, vert, 2), nr, flatshade_first),
         poly_vertex_id_for_tri_class(b, topology, prim, vert, flatshade_first));
   }`));
}
