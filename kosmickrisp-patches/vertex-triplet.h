/* SPDX-License-Identifier: MIT
 * Shared with the PerVertexKHR heap transport adapted from Mesa !45585. */
#pragma once
#include "nir_builder.h"

static inline nir_def *
kk_nir_load_primitive_corner(nir_builder *b, nir_def *address, unsigned offset,
                              unsigned components, unsigned bits)
{
   nir_def *value = nir_build_load_global(b, components, 32,
      nir_iadd_imm(b, address, offset), .align_mul = 4u);
   return bits == 32 ? value : nir_u2uN(b, value, bits);
}
