# Copyright 2025 LunarG, Inc.
# Copyright 2025 Google LLC
# SPDX-License-Identifier: MIT

libmsl_compiler_files = files(
    'nir_to_msl.c',
    'kk_fragment_shading_rate.c',
    'msl_type_inference.c',
    'msl_iomap.c',
    'msl_nir_gather_common.c',
    'msl_nir_lower_common.c',
    'msl_nir_lower_input_attachments.c',
    'msl_nir_lower_subgroups.c',
)

msl_nir_algebraic_c = custom_target(
  input : 'msl_nir_algebraic.py',
  output : 'msl_nir_algebraic.c',
  command : [
    prog_python, '@INPUT@', '-p', dir_compiler_nir,
  ],
  capture : true,
  depend_files : nir_algebraic_depends,
)


kk_soft_fp64_spv_h = custom_target(
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

libmsl_compiler = static_library(
    'msl_compiler',
    [libmsl_compiler_files, msl_nir_algebraic_c, kk_soft_fp64_spv_h],
    dependencies : [idep_nir, idep_mesautil, idep_vulkan_runtime],
    gnu_symbol_visibility: 'hidden',
    build_by_default: false,
)

idep_msl_to_nir = declare_dependency(
  link_with : libmsl_compiler,
)

