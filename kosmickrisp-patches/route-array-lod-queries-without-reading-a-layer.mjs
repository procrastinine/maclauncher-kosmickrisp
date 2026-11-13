// SPDX-License-Identifier: MIT
// Route array LOD queries without reading a layer
export const id = "route-array-lod-queries-without-reading-a-layer";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_layered_bc_nir.h", source => source
    .replace("   nir_def *zero = nir_imm_int64(b, 0);\n   nir_pop_if(b, NULL);\n   nir_def *record = nir_if_phi(b, query, zero);\n   int coordinate = nir_tex_instr_src_index(tex, nir_tex_src_coord);\n   nir_def *layer = coordinate >= 0 ? nir_channel(b, tex->src[coordinate].src.ssa, 2) : NULL;\n   bool integer_layer = tex->op == nir_texop_txf;\n   nir_def *route = kk_layered_bc_route(b, address, record, layer, true, integer_layer);\n",
      () => "   nir_def *zero = nir_imm_int64(b, 0);\n   nir_pop_if(b, NULL);\n   nir_def *record = nir_if_phi(b, query, zero);\n   /* An LOD query's coordinate has no array index. Every layer has the same\n    * size and levels, so it routes like a size query, to the first layer. */\n   int coordinate = tex->op == nir_texop_lod ? -1 :\n      nir_tex_instr_src_index(tex, nir_tex_src_coord);\n   nir_def *layer = coordinate >= 0 ? nir_channel(b, tex->src[coordinate].src.ssa, 2) : NULL;\n   bool integer_layer = tex->op == nir_texop_txf;\n   nir_def *route = kk_layered_bc_route(b, address, record, layer, true, integer_layer);\n"));
}
