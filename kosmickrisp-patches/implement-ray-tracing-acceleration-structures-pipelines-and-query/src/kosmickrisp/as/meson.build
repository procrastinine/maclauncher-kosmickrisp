# SPDX-License-Identifier: MIT
# Separate programs preserve the existing eager libkk program set. Runtime AS
# programs are instantiated only for commands that actually use them.
kk_as_spv = custom_target(
  'kk_as_spv',
  input : files('kosmickrisp-as.cl'),
  output : 'kk_as.spv',
  command : [
    prog_mesa_clc, '-o', '@OUTPUT@', '--depfile', '@DEPFILE@', '@INPUT@', '--',
    '-I' + join_paths(meson.project_source_root(), 'include'),
    '-I' + join_paths(meson.project_source_root(), 'src'),
    '-I' + join_paths(meson.project_source_root(), 'src/compiler/libcl'),
    '-I' + join_paths(meson.current_source_dir(), '.'),
    cl_args,
  ],
  depfile : 'kk_as.spv.d',
)

kk_as_shaders = custom_target(
  'kk_as_shaders',
  input : kk_as_spv,
  output : ['libkk_shaders.h', 'libkk_shaders.c'],
  command : [prog_kk_clc, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
)

# kk_clc's normal generated schema is reused without changing the compiler ABI.
# Kernel names are AS-specific; only the program-set type/count/map need aliases.
kk_as_shaders_lib = static_library(
  'kk_as_shaders',
  sources : [kk_as_shaders],
  c_args : [
    '-Dlibkk_program=kk_as_program',
    '-DLIBKK_NUM_PROGRAMS=KK_AS_NUM_PROGRAMS',
    '-Dlibkk_AppleSilicon=kk_as_AppleSilicon',
  ],
  dependencies : [idep_nir, idep_mesautil],
)

idep_kk_as_shaders = declare_dependency(
  sources : [kk_as_shaders[0]],
  include_directories : include_directories('.'),
  link_with : kk_as_shaders_lib,
)
