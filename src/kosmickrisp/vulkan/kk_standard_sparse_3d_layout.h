/* SPDX-License-Identifier: MIT */
#ifndef KK_STANDARD_SPARSE_3D_LAYOUT_H
#define KK_STANDARD_SPARSE_3D_LAYOUT_H
#include "kosmickrisp/compiler/kk_sparse_layout.h"
#define KK_STANDARD_3D_MIP_LEVELS 17
/* Only standard-shaped 3D views extend the shared prefix. The native and
 * layered-BC record sizes and every descriptor field remain unchanged. */
struct kk_standard_3d_query {
   struct kk_sparse_query_record prefix;
   uint64_t logical_view;
};
struct kk_standard_3d_view {
   uint64_t image_table;
   uint64_t targets;
   uint32_t zero_one_mask;
   uint32_t coord_scale[3];
};
/* Each tile has one logical binding, independently of native backing overlap.
 * A zero target ID keeps the existing native view; a target supplies the
 * canonical backing texture for a physical page. */
struct kk_standard_3d_tile {
   uint64_t target_id;
   uint32_t origin[3];
   uint32_t level;
   uint32_t resident;
   uint32_t reserved;
};
struct kk_standard_3d_table {
   uint32_t shape[3], first_tail;
   uint32_t extent[3], levels;
   uint32_t element_bytes, block[3];
   uint64_t entries;
   uint64_t level_page[KK_STANDARD_3D_MIP_LEVELS];
   uint64_t tail_offset_B[KK_STANDARD_3D_MIP_LEVELS];
};
_Static_assert(sizeof(struct kk_sparse_query_record) == 96, "shared query prefix");
_Static_assert(offsetof(struct kk_standard_3d_query, logical_view) == sizeof(struct kk_sparse_query_record), "3D extension follows actual prefix");
_Static_assert(sizeof(struct kk_standard_3d_query) == 104, "3D query stride");
_Static_assert(sizeof(struct kk_standard_3d_tile) == 32, "3D tile entry");
_Static_assert(sizeof(struct kk_standard_3d_view) == 32, "3D view extension");
#endif
