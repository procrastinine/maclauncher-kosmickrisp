// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'fp32-denorm-preserve';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-fp32-preserve.h', 'kosmickrisp-patches/kosmickrisp-fp32-preserve.metal'];
export function apply({edit, edits}) {
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('static void\nalu_to_msl(', '#include "kk_fp32_preserve.h"\n\nstatic void\nalu_to_msl(')
    .replace('   switch (instr->op) {\n   case nir_op_isign:', '   if (kk_fp32_preserve_alu(ctx, instr)) return;\n   switch (instr->op) {\n   case nir_op_isign:')
    .replace('   NIR_PASS(_, nir, nir_lower_frexp);', '   if (kk_fp32_preserve(nir))\n      NIR_PASS(_, nir, nir_shader_instructions_pass, kk_fp32_preserve_prepare, nir_metadata_control_flow, NULL);\n   NIR_PASS(_, nir, nir_lower_frexp);')
    .replace('   P(&ctx, "using namespace metal;\\n");', '   P(&ctx, "using namespace metal;\\n");\n   if (kk_fp32_preserve(shader)) P(&ctx, "%s", kk_fp32_preserve_msl);'));
  edits.set('src/kosmickrisp/compiler/kk_fp32_preserve.h',fs.readFileSync(new URL('./kosmickrisp-fp32-preserve.h',import.meta.url),'utf8'));
  const msl=fs.readFileSync(new URL('./kosmickrisp-fp32-preserve.metal',import.meta.url),'utf8')+'\n#pragma METAL fp math_mode(fast)\n';
  edits.set('src/kosmickrisp/compiler/kk_fp32_preserve_msl.h','static const char kk_fp32_preserve_msl[] = '+JSON.stringify(msl)+';\n');
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '.shaderDenormPreserveFloat32 = false', '.shaderDenormPreserveFloat32 = true'));
}
