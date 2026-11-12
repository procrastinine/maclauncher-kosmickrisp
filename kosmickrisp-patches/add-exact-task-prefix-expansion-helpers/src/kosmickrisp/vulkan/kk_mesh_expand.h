/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_EXPAND_H
#define KK_MESH_EXPAND_H
#include "kk_bo.h"
#include "kosmickrisp/libkk/kk_mesh_types.h"
struct kk_device;
struct kk_cmd_buffer;
/* Identical field layout in the expansion kernel; the MSL static assertion
 * uses sizeof of this host declaration. Metadata capacity is storage only. */
struct kk_mesh_expand_params {
   struct kk_mesh_params mesh;
   uint64_t root, samplers, per_draw, linked_vs, linked_fs, guard;
   uint64_t parameters, grids, range;
   uint32_t limit, capacity, local[3];
};
struct kk_mesh_exact_batch {
   struct kk_ptr batch;
   uint64_t parameters, grids, range;
   uint32_t capacity, span;
   void *icb;
};
bool kk_mesh_expand_native(struct kk_cmd_buffer *, struct kk_mesh_expand_params *,
   void **icb);
#endif
