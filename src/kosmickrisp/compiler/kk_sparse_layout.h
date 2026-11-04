/* SPDX-License-Identifier: MIT */
#ifndef KK_SPARSE_QUERY_LAYOUT_H
#define KK_SPARSE_QUERY_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include "kk-sampler-params.h"
#ifdef __cplusplus
#define KK_SPARSE_LAYOUT_ASSERT static_assert
#else
#define KK_SPARSE_LAYOUT_ASSERT _Static_assert
#endif

/* Integer image LOD and independent image-descriptor provenance tags. */
#define KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG 0x4000u
#define KK_IMAGE_QUERY_RECORD_TAG 0x8000u
#define KK_IMAGE_LOD_VALUE_MASK 0x3fffu
KK_SPARSE_LAYOUT_ASSERT((KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG & KK_IMAGE_QUERY_RECORD_TAG) == 0,
                       "independent image descriptor tags");
KK_SPARSE_LAYOUT_ASSERT((KK_IMAGE_LOD_VALUE_MASK & (KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG | KK_IMAGE_QUERY_RECORD_TAG)) == 0,
                       "image LOD excludes both tags");
KK_SPARSE_LAYOUT_ASSERT((KK_IMAGE_LOD_VALUE_MASK | KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG | KK_IMAGE_QUERY_RECORD_TAG) == UINT16_MAX,
                       "image metadata occupies one uint16");
/* A uint32 image extent has at most 32 mip levels. */
KK_SPARSE_LAYOUT_ASSERT(32u <= KK_IMAGE_LOD_VALUE_MASK,
                       "all supported image mip counts fit the LOD value");

enum kk_query_record_kind {
   KK_QUERY_RECORD_KIND_NATIVE = 0,
   KK_QUERY_RECORD_KIND_LAYERED_BC = 1,
   KK_QUERY_RECORD_KIND_STANDARD_3D = 2,
};

/* Sampled records start with ID/base LOD/level count. Storage records start
 * with ID/slice offset/slice count, matching the sliced-3D descriptor prefix;
 * their base LOD and level count are at 64/68. The appended normal plain/array
 * and wide-query array IDs serve single-layer descriptor aliases. Both records
 * occupy 96 bytes; existing prefix offsets are unchanged. */
struct kk_sparse_query_record {
   uint64_t resource_id;
   union {
      struct { uint32_t base_lod, level_count; };
      struct { uint32_t offset, count; } storage_slices;
   };
   uint64_t sampler_table;
   uint64_t face_resource_id;
   float gradient_scale[3];
   uint32_t view_type;
   uint32_t view_size[3], kind;
   uint32_t storage_base_lod, storage_level_count;
   uint64_t plain_resource_id, array_resource_id, array_query_resource_id;
};
KK_SPARSE_LAYOUT_ASSERT(sizeof(struct kk_sparse_query_record) == 96, "sparse view query record");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, plain_resource_id) == 72, "normal plain query ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, array_resource_id) == 80, "normal array query ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, array_query_resource_id) == 88, "wide array query ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, base_lod) == 8, "sampled query LOD ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, storage_slices.offset) == 8, "storage query slice offset ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, storage_slices.count) == 12, "storage query slice count ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, sampler_table) == 16, "query sampler ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, storage_base_lod) == 64, "storage query LOD ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_query_record, kind) == 60, "query record kind ABI");


enum kk_sparse_physical_border {
   KK_SPARSE_BORDER_TRANSPARENT_ZERO = 0,
   KK_SPARSE_BORDER_OPAQUE_BLACK = 1,
   KK_SPARSE_BORDER_ONE = 2,
};
enum kk_sparse_physical_reduction {
   KK_SPARSE_REDUCTION_WEIGHTED = 0,
   KK_SPARSE_REDUCTION_MIN = 1,
   KK_SPARSE_REDUCTION_MAX = 2,
};
#define KK_SPARSE_PHYSICAL_BORDER_MASK 0x3u
#define KK_SPARSE_PHYSICAL_REDUCTION_SHIFT 2u
#define KK_SPARSE_PHYSICAL_REDUCTION_MASK 0xcu
#define KK_SPARSE_PHYSICAL_FACTS_MASK 0xfu
KK_SPARSE_LAYOUT_ASSERT((KK_SPARSE_PHYSICAL_BORDER_MASK & KK_SPARSE_PHYSICAL_REDUCTION_MASK) == 0,
                       "independent native sampler facts");
static inline uint32_t
kk_sparse_sampler_facts_encode(enum kk_sparse_physical_border border,
                               enum kk_sparse_physical_reduction reduction)
{
   assert(border <= KK_SPARSE_BORDER_ONE && reduction <= KK_SPARSE_REDUCTION_MAX);
   return border | (reduction << KK_SPARSE_PHYSICAL_REDUCTION_SHIFT);
}
static inline enum kk_sparse_physical_border
kk_sparse_sampler_facts_border(uint32_t facts)
{
   return (enum kk_sparse_physical_border)(facts & KK_SPARSE_PHYSICAL_BORDER_MASK);
}
static inline enum kk_sparse_physical_reduction
kk_sparse_sampler_facts_reduction(uint32_t facts)
{
   return (enum kk_sparse_physical_reduction)
      ((facts & KK_SPARSE_PHYSICAL_REDUCTION_MASK) >> KK_SPARSE_PHYSICAL_REDUCTION_SHIFT);
}
#define KK_SPARSE_SAMPLER_STRIDE 32u
#define KK_SPARSE_NATIVE_SAMPLER_HEAD (MSL_MAX_SAMPLERS * KK_SPARSE_SAMPLER_STRIDE)
struct kk_sparse_native_sampler_record {
   uint64_t next_address;
   uint64_t source_id;
   struct kk_sampler_params params;
   uint32_t physical_facts;
   uint32_t reserved[3];
};
KK_SPARSE_LAYOUT_ASSERT(sizeof(struct kk_sparse_native_sampler_record) == 64, "native sampler record ABI");
#ifdef __cplusplus
KK_SPARSE_LAYOUT_ASSERT(alignof(struct kk_sparse_native_sampler_record) == 16, "native sampler record alignment");
#else
KK_SPARSE_LAYOUT_ASSERT(_Alignof(struct kk_sparse_native_sampler_record) == 16, "native sampler record alignment");
#endif
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_native_sampler_record, next_address) == 0, "native record link ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_native_sampler_record, source_id) == 8, "native sampler identity ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_native_sampler_record, params) == 16, "native sampler parameters ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_native_sampler_record, physical_facts) == 48, "native sampler facts ABI");
KK_SPARSE_LAYOUT_ASSERT(offsetof(struct kk_sparse_native_sampler_record, reserved) == 52, "native sampler reserved ABI");
enum kk_sparse_sampler_row_word {
   KK_SPARSE_ROW_NATIVE_PARAMS_WORD = 0,
   KK_SPARSE_ROW_PHYSICAL_FACTS_WORD = 1,
   KK_SPARSE_ROW_BLACK_TAP_WORD = 2,
   KK_SPARSE_ROW_WHITE_TAP_WORD = 3,
   KK_SPARSE_ROW_ENTRY_WORDS = 4,
};

#undef KK_SPARSE_LAYOUT_ASSERT
#endif
