// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'cooperative-matrix-tile';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-matrix-tile.h'];
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/compiler/kk_matrix_tile.h', fs.readFileSync(new URL('./kosmickrisp-matrix-tile.h', import.meta.url), 'utf8'));
  edit('src/compiler/nir/nir_intrinsics.py', s => s + '\n# Native Metal 8x8 fragments compose opaque 8x8 and 16x16 matrices.\n' +
    'intrinsic("load_cmat_kk", src_comp=[1, 1, 1], dest_comp=2, bit_sizes=[8,16,32], indices=[MATRIX_LAYOUT, MEMORY_MODES], flags=[CAN_ELIMINATE])\n' +
    'intrinsic("store_cmat_kk", src_comp=[1, 2, 1, 1], indices=[MATRIX_LAYOUT, MEMORY_MODES])\n' +
    'intrinsic("cmat_mma_kk", src_comp=[2, 2, 2], dest_comp=2, bit_sizes=[16,32], flags=[CAN_ELIMINATE])\n');
  edit('src/compiler/nir/nir_intrinsics.py', s => s + '\nintrinsic("load_cmat_map_kk", src_comp=[], dest_comp=1, bit_sizes=[64], flags=[CAN_ELIMINATE, CAN_REORDER])\n');
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('static void\nintrinsic_to_msl(', '#include "kk_matrix_tile.h"\n\nstatic void\nintrinsic_to_msl(')
    .replace('intrinsic_to_msl(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *instr)\n{',
      'intrinsic_to_msl(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *instr)\n{\n   if (kk_emit_matrix_tile(ctx, instr)) return;')
    .replace('      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);',
      '      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);\n      NIR_PASS(progress, nir, nir_lower_phis_to_scalar, kk_matrix_phi_width, NULL);')
    .replace('   NIR_PASS(_, nir, nir_lower_global_vars_to_local);',
      '   NIR_PASS(_, nir, nir_lower_global_vars_to_local);\n   kk_lower_matrix_tile(nir);')
    .replace('   P(&ctx, "#include <metal_stdlib>\\n");',
      '   P(&ctx, "#include <metal_stdlib>\\n");\n   if (shader->info.stage == MESA_SHADER_COMPUTE && shader->info.cs.has_cooperative_matrix)\n      P(&ctx, "#include <metal_simdgroup_matrix>\\n");'));
  edit('src/kosmickrisp/compiler/msl_type_inference.c', s => s.replace(
    '   case nir_intrinsic_load_global:\n   case nir_intrinsic_load_push_constant:',
    '   case nir_intrinsic_load_cmat_kk:\n' +
    '      set_type(types, &instr->def, instr->def.bit_size == 8 ? TYPE_UINT : TYPE_FLOAT);\n' +
    '      set_type(types, &instr->src[0], TYPE_UINT);\n' +
    '      set_type(types, &instr->src[1], TYPE_UINT);\n' +
    '      set_type(types, &instr->src[2], TYPE_UINT);\n' +
    '      break;\n' +
    '   case nir_intrinsic_store_cmat_kk:\n' +
    '      set_type(types, &instr->src[0], TYPE_UINT);\n' +
    '      set_type(types, &instr->src[1], instr->src[1].ssa->bit_size == 8 ? TYPE_UINT : TYPE_FLOAT);\n' +
    '      set_type(types, &instr->src[2], TYPE_UINT);\n' +
    '      set_type(types, &instr->src[3], TYPE_UINT);\n' +
    '      break;\n' +
    '   case nir_intrinsic_cmat_mma_kk:\n' +
    '      set_type(types, &instr->def, TYPE_FLOAT);\n' +
    '      for (unsigned i = 0; i < 3; i++) set_type(types, &instr->src[i], TYPE_FLOAT);\n' +
    '      break;\n' +
    '   case nir_intrinsic_load_global:\n   case nir_intrinsic_load_push_constant:'));
}
