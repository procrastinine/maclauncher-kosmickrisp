/* SPDX-License-Identifier: MIT */
#ifndef KK_GS_MESH_H
#define KK_GS_MESH_H

/* Addressed through the existing per-draw vertex_params pointer, only for a
 * selected internal object/mesh GS draw. Not an application ABI or cache. */
struct kk_gs_mesh_draw {
   uint64_t index;
   uint32_t index_count;
   uint32_t index_size;
   uint32_t first_vertex;
   int32_t base_vertex;
   uint32_t first_instance;
};

#endif
