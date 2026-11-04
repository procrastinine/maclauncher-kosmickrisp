/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_TYPES_H
#define KK_FRAGMENT_SHADING_RATE_TYPES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kk_mesh_types.h"
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
   U32 result_stride; U32 sample_mask; U64 original_per_draw; \
   U32 origin_x; U32 origin_y; \
}; \
struct kk_vrs_coverage { U32 mask; F32 z; F32 reciprocal_w; U32 reserved; }; \
struct kk_vrs_result { F32 color[4]; U32 mask; U32 api_primitive; U32 rate; U32 reserved; }; \
struct kk_vrs_mesh_prepare { \
   U64 context; U64 job; U64 outputs; U64 output_bytes; U64 grid; U64 per_draw; \
   U32 capacity; U32 group_stride; U32 vertices; U32 vertex_stride; \
   U32 primitives; U32 primitive_stride; U32 indices; U32 vertices_per_primitive; \
   U32 max_vertices; U32 max_primitives; U32 provoking_last; \
   U32 id_source; U32 id_offset; U32 rate_source; U32 rate_offset; U32 advance; \
};
KK_VRS_LAYOUT(uint64_t, uint32_t, float)
/* Only the software coarse transaction extends its existing PER_DRAW context.
 * envelope is the real immutable DS packet, not a copied transaction state.
 * capture_primitive is its capture-local storage job, never shader PrimitiveID.
 * Each ordered leaf executes region_base0,4,8,12 before reusing its capture. */
struct kk_vrs_transaction_context {
   struct kk_vrs_context base;
   uint64_t envelope;
   uint32_t capture_primitive;
   uint32_t region_base;
};
_Static_assert(offsetof(struct kk_vrs_transaction_context, envelope) == 128,
   "software transaction context suffix");
_Static_assert(sizeof(struct kk_vrs_transaction_context) == 144,
   "software transaction context size");

/* The prefix is the Mesh owner's job type. Cursor storage belongs to one
 * original retained batch and never carries shader-visible PrimitiveID. */
#define KK_VRS_MESH_JOBS_LAYOUT(U32, Job) \
enum kk_vrs_mesh_jobs_error { KK_VRS_JOBS_OK, KK_VRS_JOBS_BAD_GRID, \
   KK_VRS_JOBS_BAD_SPAN, KK_VRS_JOBS_BAD_COUNTS }; \
struct kk_vrs_mesh_jobs_state { Job job; U32 next_group; U32 next_slot; \
   U32 finished; U32 error; };
KK_VRS_MESH_JOBS_LAYOUT(uint32_t, struct kk_mesh_retained_job)

#define KK_VRS_STRING_(...) #__VA_ARGS__
#define KK_VRS_STRING(...) KK_VRS_STRING_(__VA_ARGS__)
#define KK_VRS_MSL_LAYOUT KK_VRS_STRING(KK_VRS_LAYOUT(ulong, uint, float))
#define KK_VRS_MSL_MESH_JOBS_LAYOUT KK_VRS_STRING(KK_VRS_MESH_JOBS_LAYOUT(uint, kk_mesh_retained_job))
_Static_assert(sizeof(struct kk_vrs_mesh_jobs_state) == 24, "GPU Mesh cursor layout");
_Static_assert(offsetof(struct kk_vrs_mesh_jobs_state, job) == 0, "original Mesh job prefix");
_Static_assert(offsetof(struct kk_vrs_mesh_prepare, advance) == 108, "preparation mode uses existing padding");
_Static_assert(sizeof(struct kk_vrs_mesh_prepare) == 112, "Mesh preparation layout");
_Static_assert(sizeof(struct kk_vrs_context) == 128, "coarse context layout");
_Static_assert(offsetof(struct kk_vrs_context, sample_mask) == 108, "sample mask uses existing padding");
_Static_assert(offsetof(struct kk_vrs_context, origin_x) == 120, "window origin suffix");
_Static_assert(sizeof(struct kk_vrs_coverage) == 16, "coverage layout");
_Static_assert(sizeof(struct kk_vrs_result) == 32, "result layout");
#endif
