/* SPDX-License-Identifier: MIT */
#ifndef KK_TESS_GRID_H
#define KK_TESS_GRID_H
#include "compiler/libcl/libcl.h"
/* Metal indirect thread dispatch: exact grid followed by threadgroup size. */
struct kk_tess_grid {
   uint32_t threads[3];
   uint32_t group[3];
} PACKED;
static_assert(sizeof(struct kk_tess_grid) == 6 * sizeof(uint32_t),
              "indirect tessellation grid must contain six words");
#endif
