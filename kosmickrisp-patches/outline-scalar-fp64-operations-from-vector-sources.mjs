// SPDX-License-Identifier: MIT
// Outline scalar FP64 operations from vector sources
export const id = "outline-scalar-fp64-operations-from-vector-sources";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/kk_soft_fp64.h", source => source
    .replace("      return false;\n   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; ++i) {\n      unsigned bits = alu->src[i].src.ssa->bit_size;\n      if (nir_src_num_components(alu->src[i].src) != 1 || (bits != 32 && bits != 64))\n         return false;\n   }\n   return true;\n",
      () => "      return false;\n   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; ++i) {\n      unsigned bits = alu->src[i].src.ssa->bit_size;\n      /* Scalarization selects one swizzle from a possibly vector source. */\n      if (nir_ssa_alu_instr_src_components(alu, i) != 1 ||\n          (bits != 32 && bits != 64))\n         return false;\n   }\n   return true;\n"));
}
