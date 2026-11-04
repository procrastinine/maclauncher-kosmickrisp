/* SPDX-License-Identifier: MIT */
#ifndef KK_SOFT_FP64_LIBRARY_H
#define KK_SOFT_FP64_LIBRARY_H
#include "nir.h"

/* Immutable library; inlining clones functions into the destination shader. */
const nir_shader *kk_soft_fp64_library(const nir_shader_compiler_options *options);
#endif
