// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'cooperative-matrix-fp8';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-float8.h',
  'kosmickrisp-patches/kosmickrisp-float8.metal',
  'kosmickrisp-patches/kosmickrisp-cooperative-matrix.h',
];
export function apply({edit, edits}) {
  const file = name => fs.readFileSync(new URL('./' + name, import.meta.url), 'utf8');
  edits.set('src/kosmickrisp/compiler/kk_float8.h', file('kosmickrisp-float8.h'));
  edits.set('src/kosmickrisp/vulkan/kk_cooperative_matrix.h', file('kosmickrisp-cooperative-matrix.h'));
  edits.set('src/kosmickrisp/compiler/kk_float8_msl.h',
    '/* Copyright 2025 Valve Corporation; SPDX-License-Identifier: MIT */\n' +
    'static const char kk_float8_msl[] = ' + JSON.stringify(file('kosmickrisp-float8.metal')) + ';\n');
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('static void\nalu_to_msl(', '#include "kk_float8.h"\n#include "kk_float8_msl.h"\n\nstatic void\nalu_to_msl(')
    .replace('alu_to_msl(struct nir_to_msl_ctx *ctx, nir_alu_instr *instr)\n{',
      'alu_to_msl(struct nir_to_msl_ctx *ctx, nir_alu_instr *instr)\n{\n   if (kk_emit_float8_alu(ctx, instr)) return;')
    .replace('   P(&ctx, "using namespace metal;\\n");',
      '   P(&ctx, "using namespace metal;\\n");\n   if (kk_uses_float8(shader)) P(&ctx, "%s", kk_float8_msl);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    '         uint32_t base_group[3];',
    '         uint32_t base_group[3];\n' +
    '         /* Coordinates, not a lane map: a native load owns distribution. */\n' +
    '         uint16_t matrix_coordinates[64];'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s
    .replace('#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "util/half_float.h"')
    .replace('   vk_command_buffer_begin(&cmd->vk, pBeginInfo);',
      '   vk_command_buffer_begin(&cmd->vk, pBeginInfo);\n' +
      '   for (unsigned i = 0; i < 64; i++)\n' +
      '      cmd->state.cs.descriptors.root.cs.matrix_coordinates[i] = _mesa_float_to_half((float)i);'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    '   case nir_intrinsic_load_base_workgroup_id:',
    '   case nir_intrinsic_load_cmat_map_kk: {\n' +
    '      b->cursor = nir_before_instr(&intrin->instr);\n' +
    '      nir_def *root = nir_load_buffer_ptr_kk(b, 1, 64, .binding = 0);\n' +
    '      nir_def_rewrite_uses(&intrin->def, nir_iadd_imm(b, root,\n' +
    '         kk_root_descriptor_offset(cs.matrix_coordinates)));\n' +
    '      nir_instr_remove(&intrin->instr);\n' +
    '      return true;\n' +
    '   }\n\n' +
    '   case nir_intrinsic_load_base_workgroup_id:'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('   *ext = (struct vk_device_extension_table){',
      '   *ext = (struct vk_device_extension_table){\n' +
      '      .KHR_cooperative_matrix = pdev->info.gpu_apple_family >= 7,\n' +
      '      .EXT_shader_float8 = true,')
    .replace('      .shaderFloat16 = true,',
      '      .shaderFloat16 = true,\n' +
      '      .cooperativeMatrix = pdev->info.gpu_apple_family >= 7,\n' +
      '      .cooperativeMatrixRobustBufferAccess = false,\n' +
      '      .shaderFloat8 = true,\n' +
      '      .shaderFloat8CooperativeMatrix = pdev->info.gpu_apple_family >= 7,')
    .replace('   *properties = (struct vk_properties){',
      '   *properties = (struct vk_properties){\n' +
      '      .cooperativeMatrixSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT,')
    + '\n#include "kk_cooperative_matrix.h"\n');
}
