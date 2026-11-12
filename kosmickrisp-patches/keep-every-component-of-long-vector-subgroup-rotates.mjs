// SPDX-License-Identifier: MIT
// Keep every component of long-vector subgroup rotates
export const id = "keep-every-component-of-long-vector-subgroup-rotates";
export function apply({edit, edits}) {
  edit("src/compiler/spirv/vtn_subgroup.c", source => source
    .replace("\n      struct vtn_ssa_value *value = vtn_ssa_value(b, w[4]);\n      struct vtn_ssa_value *delta = vtn_ssa_value(b, w[5]);\n      vtn_push_nir_ssa(b, w[2],\n         vtn_build_subgroup_instr(b, nir_intrinsic_rotate,\n                                  value, NULL, delta->def, cluster_size, 0)->def);\n      break;\n",
      () => "\n      struct vtn_ssa_value *value = vtn_ssa_value(b, w[4]);\n      struct vtn_ssa_value *delta = vtn_ssa_value(b, w[5]);\n      vtn_push_ssa_value(b, w[2],\n         vtn_build_subgroup_instr(b, nir_intrinsic_rotate,\n                                  value, NULL, delta->def, cluster_size, 0)->def);\n      break;\n")
    .replace("      struct vtn_ssa_value *delta = vtn_ssa_value(b, w[5]);\n      vtn_push_ssa_value(b, w[2],\n         vtn_build_subgroup_instr(b, nir_intrinsic_rotate,\n                                  value, NULL, delta->def, cluster_size, 0)->def);\n      break;\n   }\n\n",
      () => "      struct vtn_ssa_value *delta = vtn_ssa_value(b, w[5]);\n      vtn_push_ssa_value(b, w[2],\n         vtn_build_subgroup_instr(b, nir_intrinsic_rotate,\n                                  value, NULL, delta->def, cluster_size, 0));\n      break;\n   }\n\n"));
}
