/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BOUNDS_TYPES_H
#define KK_DEPTH_BOUNDS_TYPES_H
#include <stdint.h>
#include "kk_interlock_envelope.h"

/* Extended transactions carry runtime input mappings. The ordinary shared
 * envelope retains its original upload size. */
struct kk_depth_bounds_envelope {
   struct kk_interlock_envelope base;
   uint32_t input_depth_index, input_stencil_index;
   uint32_t input_color_indices[8];
};
struct kk_depth_bounds_capture_context {
   uint64_t env, original_per_draw, job;
   uint32_t max_primitives, reserved;
};
#endif
