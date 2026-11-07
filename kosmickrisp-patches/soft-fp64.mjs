// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import {addLicense} from '../package-licences.mjs';
import {adaptSoftFp64Library} from './soft-fp64-library.mjs';

export const id = 'soft-fp64';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-soft-fp64.h',
  'kosmickrisp-patches/soft-fp64-library.mjs',
  'kosmickrisp-patches/kosmickrisp-soft-fp64-extra.glsl',
  'kosmickrisp-patches/kosmickrisp-soft-fp64-subgroups.h',
];

export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('.shaderFmaFloat64 = false', '.shaderFmaFloat64 = true')
    .replace('      .shaderInt64 = true,', '      .shaderInt64 = true,\n      .shaderFloat64 = true,')
    .replace('.shaderSignedZeroInfNanPreserveFloat64 = false', '.shaderSignedZeroInfNanPreserveFloat64 = true')
    .replace('.shaderDenormPreserveFloat64 = false', '.shaderDenormPreserveFloat64 = true')
    .replace('.shaderRoundingModeRTEFloat64 = false', '.shaderRoundingModeRTEFloat64 = true'));

  edit('src/compiler/glsl/float64.glsl', adaptSoftFp64Library);
  edits.set('src/compiler/nir/kk_soft_fp64_subgroups.h',
    fs.readFileSync(new URL('./kosmickrisp-soft-fp64-subgroups.h', import.meta.url), 'utf8'));
  edit('src/compiler/nir/nir_lower_subgroups.c', s => {
    const start = s.indexOf('static nir_def *\nbuild_scan_reduce(');
    const end = s.indexOf('static nir_def *\nbuild_cluster_mask(', start);
    if (start < 0 || end < 0) throw Error('Soft-FP64 scan function anchor missing');
    s = s.slice(0, start) + '#include "kk_soft_fp64_subgroups.h"\n\n' + s.slice(end);
    if (!s.includes('   nir_def *full, *partial;')) throw Error('Soft-FP64 scan selection anchor missing');
    return s.replace('   nir_def *full, *partial;',
      '   if (options->subgroup_size == 32 && intrin->def.bit_size == 64 &&\n' +
      '       nir_alu_type_get_base_type(nir_op_infos[red_op].output_type) == nir_type_float) {\n' +
      '      if (cluster_size < subgroup_size)\n' +
      '         mask = nir_iand(b, mask, build_cluster_mask(b, cluster_size, options));\n' +
      '      return build_scan_reduce(b, intrin->intrinsic, red_op,\n' +
      '                               intrin->src[0].ssa, mask, cluster_size, options);\n' +
      '   }\n\n   nir_def *full, *partial;');
  });
  edit('src/compiler/nir/nir_softfloat.h', s => s.replace(
    'n_type = n_type | instr->src[0].src.ssa->bit_size;',
    'n_type = n_type | instr->src[i].src.ssa->bit_size;'));
  edits.set('src/kosmickrisp/compiler/kk_soft_fp64.h',
    fs.readFileSync(new URL('./kosmickrisp-soft-fp64.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/compiler/meson.build', s => s
    .replace('libmsl_compiler = static_library(', `kk_soft_fp64_spv_h = custom_target(
  'kk_soft_fp64_spv.h',
  input : [glsl2spirv, float64_glsl_file],
  output : 'kk_soft_fp64_spv.h',
  command : [
    prog_python, '@INPUT@', '@OUTPUT@',
    find_program('glslangValidator', 'glslang', native : true),
    '--vn', 'kk_soft_fp64_spv',
    '--glsl-version', '450',
    '-Olib',
    glslang_depfile,
  ],
  depfile : 'kk_soft_fp64_spv.h.d',
)

libmsl_compiler = static_library(`)
    .replace('[libmsl_compiler_files, msl_nir_algebraic_c]',
      '[libmsl_compiler_files, msl_nir_algebraic_c, kk_soft_fp64_spv_h]'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('static bool\nkk_scalarize_filter(',
      '#include "kk_soft_fp64.h"\n\nstatic bool\nkk_scalarize_filter(')
    .replace('   NIR_PASS(_, nir, nir_lower_frexp);',
      '   kk_soft_fp64_lower(nir);\n   NIR_PASS(_, nir, nir_lower_frexp);'));
}

export function collectSoftFp64License(root) {
  addLicense(root, 'Berkeley SoftFloat Release 3e',
    new URL('./kosmickrisp-soft-fp64-LICENSE.txt', import.meta.url));
}
