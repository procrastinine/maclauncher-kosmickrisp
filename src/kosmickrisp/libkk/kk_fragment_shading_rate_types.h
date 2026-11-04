/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_TYPES_H
#define KK_FRAGMENT_SHADING_RATE_TYPES_H
#include <stdint.h>
#include <stddef.h>
/* One layout for the native encoder and compiler. The context occupies the
 * start of PER_DRAW on separate coarse/coverage/replay stage tables. */
#define KK_VRS_LAYOUT(U64, U32, F32) \
struct kk_vrs_context { \
   U64 coverage; U64 results; U64 diagnostics; \
   U32 width; U32 height; U32 fragment_width; U32 fragment_height; \
   U32 tile_width; U32 tile_height; U32 api_primitive; U32 reserved; \
   U64 rate_texture; U32 rate_image_width; U32 rate_image_height; \
   U32 texel_width; U32 texel_height; U32 primitive_rate; \
   U32 combiner0; U32 combiner1; U32 has_attachment; U64 pixel_results; \
   U32 result_stride; U64 original_per_draw; \
}; \
struct kk_vrs_coverage { U32 mask; F32 z; F32 reciprocal_w; U32 reserved; }; \
struct kk_vrs_result { F32 color[4]; U32 mask; U32 api_primitive; U32 rate; U32 reserved; }; \
struct kk_vrs_mesh_prepare { \
   U64 context; U64 job; U64 outputs; U64 output_bytes; U64 grid; U64 per_draw; \
   U32 capacity; U32 group_stride; U32 vertices; U32 vertex_stride; \
   U32 primitives; U32 primitive_stride; U32 indices; U32 vertices_per_primitive; \
   U32 max_vertices; U32 max_primitives; U32 provoking_last; \
   U32 id_source; U32 id_offset; U32 rate_source; U32 rate_offset; \
};
KK_VRS_LAYOUT(uint64_t, uint32_t, float)
#define KK_VRS_STRING_(x) #x
#define KK_VRS_STRING(x) KK_VRS_STRING_(x)
#define KK_VRS_MSL_LAYOUT KK_VRS_STRING(KK_VRS_LAYOUT(ulong, uint, float))
_Static_assert(sizeof(struct kk_vrs_mesh_prepare) == 112, "Mesh preparation layout");
_Static_assert(sizeof(struct kk_vrs_context) == 120, "coarse context layout");
_Static_assert(sizeof(struct kk_vrs_coverage) == 16, "coverage layout");
_Static_assert(sizeof(struct kk_vrs_result) == 32, "result layout");
#endif
