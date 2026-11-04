/* SPDX-License-Identifier: MIT */
#pragma once
/* backend_flags bit zero remains the upstream clamp-to-zero selector. */
#define KK_SAMPLE_HELPER (1u << 1)
#define KK_SAMPLE_MINMAX (1u << 2)
#define KK_SAMPLE_BORDER (1u << 3)
#define KK_SAMPLE_CUSTOM (1u << 4)
#define KK_SAMPLE_MIN (1u << 5)
#define KK_SAMPLE_MAX (1u << 6)
#define KK_SAMPLE_BORDER_KNOWN (1u << 7)

/* Shader backend flags, distinct from sampler descriptor/record flags. */
#define KK_SAMPLE_VIRTUAL (1u << 8)
#define KK_SAMPLE_METADATA (1u << 9)
#define KK_SAMPLE_LOWER_NATIVE_BIAS (1u << 10)
#define KK_SAMPLE_NATIVE_CUBE_LOD (1u << 11)
#define KK_SAMPLE_SPARSE (1u << 12)
#define KK_SAMPLE_SPARSE_POINT (1u << 13)
#define KK_SAMPLE_METADATA_SHIFT 16
#define KK_SAMPLE_METADATA_MASK (7u << KK_SAMPLE_METADATA_SHIFT)

/* Defer sampler bias until the actual native/emulated descriptor dispatch.
 * This bit is consumed by descriptor-aware lowering, before MSL emission. */
#define KK_SAMPLE_NATIVE_DISPATCH (1u << 19)
_Static_assert((KK_SAMPLE_NATIVE_DISPATCH & KK_SAMPLE_METADATA_MASK) == 0,
               "native dispatch is independent of metadata selectors");

/* Descriptor-aware dispatch reaches this helper only for emulating samplers.
 * Its one out-of-line entry owns the inline footprint/border implementation. */
#define KK_SAMPLE_EMULATION_ONLY (1u << 20)
_Static_assert((KK_SAMPLE_EMULATION_ONLY &
                (KK_SAMPLE_METADATA_MASK | KK_SAMPLE_NATIVE_DISPATCH)) == 0,
               "emulation entry is independent of deferred dispatch metadata");

enum kk_sampler_metadata_value {
   KK_SAMPLER_META_BIAS,
   KK_SAMPLER_META_MIN_LOD,
   KK_SAMPLER_META_MAX_LOD,
   KK_SAMPLER_META_MIPMAP,
   KK_SAMPLER_META_VIRTUAL,
   KK_SAMPLER_META_QUERY_BIAS,
   /* fp32 dimensionless ratio: 1 disabled, original maximum enabled. */
   KK_SAMPLER_META_MAX_ANISOTROPY,
   /* Original VkSamplerReductionMode, exactly represented as fp32 0/1/2. */
   KK_SAMPLER_META_REDUCTION_MODE,
};
_Static_assert((KK_SAMPLER_META_REDUCTION_MODE << KK_SAMPLE_METADATA_SHIFT) <= KK_SAMPLE_METADATA_MASK,
               "sampler metadata fits the existing selector");

_Static_assert((KK_SAMPLE_SPARSE & (KK_SAMPLE_NATIVE_CUBE_LOD | KK_SAMPLE_SPARSE_POINT | KK_SAMPLE_METADATA_MASK)) == 0,
               "sparse backend flag is independent");
_Static_assert((KK_SAMPLE_SPARSE_POINT & (KK_SAMPLE_NATIVE_CUBE_LOD | KK_SAMPLE_METADATA_MASK)) == 0,
               "sparse point backend flag is independent");
