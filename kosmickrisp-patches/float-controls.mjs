// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

export const id = 'float-controls';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-float-controls.h',
];

// Fixed Apple GPU denorm behavior follows Mesa Honeykrisp, MIT:
// https://gitlab.freedesktop.org/mesa/mesa.git
// 4f071f891d63396c799b267172c19ba6b0301682,
// src/asahi/vulkan/hk_physical_device.c (Vulkan 1.2 properties).
// Kosmic's existing per-operation MSL relaxed/safe pragmas retain the
// requested signed-zero, infinity and NaN behavior with fast library math.
function adaptFloatControlSources(sourceRoot, edits) {
  const file = 'src/kosmickrisp/vulkan/kk_physical_device.c';
  const before = edits.get(file) ?? fs.readFileSync(path.join(sourceRoot, file), 'utf8');
  edits.set(file, before
    .replace('.denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE',
      '.denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL')
    .replace('.roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE',
      '.roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL')
    .replace('.shaderDenormPreserveFloat16 = false', '.shaderDenormPreserveFloat16 = true')
    .replace('.shaderDenormFlushToZeroFloat32 = false', '.shaderDenormFlushToZeroFloat32 = true'));
  const compiler = 'src/kosmickrisp/compiler/nir_to_msl.c';
  const msl = edits.get(compiler) ?? fs.readFileSync(path.join(sourceRoot, compiler), 'utf8');
  edits.set(compiler, msl
    .replace('   case nir_op_iabs:',
      '   case nir_op_fcanonicalize:\n' +
      '      alu_src_to_msl(ctx, instr, 0);\n      break;\n' +
      '   case nir_op_iabs:')
    .replace('static const char *\nalu_fp_math_mode_pragma(',
      '#include "kosmickrisp-float-controls.h"\n\nstatic const char *\nalu_fp_math_mode_pragma(')
    .replace('   unsigned fp_math_ctrl = alu->fp_math_ctrl;',
      '   if (kk_float_control_flush_result(ctx, alu))\n      return "safe";\n\n   unsigned fp_math_ctrl = alu->fp_math_ctrl;')
    .replace('      alu_to_msl(ctx, alu);\n      P(ctx, ";\\n");',
      '      alu_to_msl(ctx, alu);\n      P(ctx, ";\\n");\n' +
      '      if (kk_float_control_flush_result(ctx, alu))\n' +
      '         kk_float_control_flush_msl(ctx, alu);'));
  edits.set('src/kosmickrisp/compiler/kosmickrisp-float-controls.h',
    fs.readFileSync(new URL('./kosmickrisp-float-controls.h', import.meta.url), 'utf8'));
}

// Preserve push-data's single uploader and its stage-filtered constant tail.
// The existing root's dynamic table grows; no second root layout is added.
function adaptDynamicBufferSources(sourceRoot, edits) {
  for (const [file, apply] of [
    ['vulkan/kk_private.h', s => s.replace(
      '#define KK_MAX_DYNAMIC_BUFFERS           64', '#define KK_MAX_DYNAMIC_BUFFERS           128')],
    ['vulkan/kk_descriptor_set_layout.c', s => s.replaceAll(
      'uint8_t dynamic_buffer_count = 0;', 'uint32_t dynamic_buffer_count = 0;')],
  ]) {
    const name = 'src/kosmickrisp/' + file;
    edits.set(name, apply(edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8')));
  }
}

// Extend the count supported by the existing heap allocator and residency set.
function adaptAllocationLimitSources(sourceRoot, edits) {
  const name = 'src/kosmickrisp/vulkan/kk_physical_device.c';
  const before = edits.get(name) ?? fs.readFileSync(path.join(sourceRoot, name), 'utf8');
  edits.set(name, before.replace('.maxMemoryAllocationCount = 4096',
    '.maxMemoryAllocationCount = 8192'));
}

export function apply({base, edits}) {
  adaptFloatControlSources(base, edits);
  adaptDynamicBufferSources(base, edits);
  adaptAllocationLimitSources(base, edits);
}
