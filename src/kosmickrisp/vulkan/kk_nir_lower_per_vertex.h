/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where native pre-raster triangle values cannot preserve the interface and
 * corner ordering, pass the three corners as flat varyings or heap records.
 */
struct kk_per_vertex_map {
   /* Emulation is active: draws are issued as non-indexed triangle lists with
    * one vertex shader invocation per triangle corner. */
   bool active;
   /* Independent non-LTO stage ABI. A producer request needs only this flag;
    * VS lowering derives its complete layout without a fragment map. */
   bool canonical_heap;
   bool projected_basis;
   /* Varying slots read with PerVertexKHR by the fragment shader. */
   uint64_t locations;
   /* Ordinary locations sharing the same per-corner heap storage. */
   uint64_t buffered_locations;
   /* Producer outputs unused by rasterization, retained in any Poly capture. */
   uint64_t unused_locations;
   /* Flat varying slots, written with the provoking vertex value. */
   uint64_t flat_locations;
   /* Flat slot holding the value of vertex N for each location. */
   uint8_t slot[VARYING_SLOT_MAX][3];
   /* Smooth and noperspective slots replacing the barycentric built-ins, 0
    * if unused. */
   uint8_t barycentric_slot[2];
   /* Not enough free varying slots for the emulation: PerVertexKHR values go
    * through a heap buffer instead, whose address is in buffer_slot, and the
    * ordinary inputs move through the same buffer to make room for the
    * required native interpolants. */
   bool buffer;
   uint8_t buffer_slot;
   /* The fragment shader needs the emulation, but not even the buffer
    * address fits in the varying slots. */
   bool out_of_slots;
};

enum kk_barycentric_usage {
   KK_BARYCENTRIC_NONE,
   /* Evaluated at the pixel center only: native Metal works. */
   KK_BARYCENTRIC_PIXEL,
   /* Sample, centroid or offset: needs the emulation. */
   KK_BARYCENTRIC_EXACT,
};

bool kk_nir_needs_interface_packing(nir_shader *fs, nir_shader *prev);

enum kk_barycentric_usage kk_nir_barycentric_usage(nir_shader *fs);

/* prev == NULL selects the canonical ABI when force or interface pressure
 * requires emulation. Linked/LTO callers keep passing their previous NIR.
 * The caller initializes map to zero; a false return leaves it inactive. */
bool kk_nir_apply_fs_per_vertex_map(nir_shader *fs, struct kk_per_vertex_map *map);

bool kk_nir_lower_fs_per_vertex_inputs(nir_shader *fs, nir_shader *prev,
                                       bool force,
                                       struct kk_per_vertex_map *map);


bool kk_nir_lower_mesh_flat_inputs(nir_shader *fs, nir_shader *mesh,
                                   struct kk_per_vertex_map *map);

bool kk_nir_lower_vs_per_vertex_outputs(nir_shader *vs,
                                        const struct kk_per_vertex_map *map);

#ifdef __cplusplus
} /* extern C */
#endif
