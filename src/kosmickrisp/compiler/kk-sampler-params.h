/* SPDX-License-Identifier: MIT
 * Shared immutable sampler parameters and Metal declarations.
 * One field/flag list feeds host layout and generated Metal declarations.
 */
#ifndef KK_SAMPLER_PARAMETERS_SHARED_H
#define KK_SAMPLER_PARAMETERS_SHARED_H

#include <stddef.h>
#include <stdint.h>

enum kk_sampler_params_lock_context {
   KK_SAMPLER_PARAMS_UNLOCKED,
   KK_SAMPLER_PARAMS_HEAP_LOCK_HELD,
};

#define KK_SAMPLER_FILTER_VIRTUAL (1u << 3)
/* Original parameter address presence is independent of sampling dispatch. */
#define KK_SAMPLER_FILTER_PARAMS_POINTER (1u << 4)
#define KK_SAMPLER_PARAMS_ALIGNMENT 16u
#define KK_SAMPLER_PARAMS_STRIDE 32u

/* Record flags describe the original state independently of descriptor flags. */
#define KK_SAMPLER_PARAMS_FLAG_LIST(F) \
   F(KK_SAMPLER_PARAMS_MIPMAP_LINEAR, 0x00000001u) \
   F(KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE, 0x00000002u)

#define KK_SAMPLER_PARAMS_FIELD_LIST(F32, U32) \
   F32(min_lod) \
   F32(max_lod) \
   F32(lod_bias) \
   F32(max_anisotropy) \
   U32(flags) \
   U32(compare_op) \
   U32(reduction_mode) \
   U32(reserved2)

#define KK_PARAMS_FLAG(name, value) name = value,
enum kk_sampler_params_flag {
   KK_SAMPLER_PARAMS_FLAG_LIST(KK_PARAMS_FLAG)
};
#undef KK_PARAMS_FLAG

#define KK_PARAMS_FLOAT(name) float name;
#define KK_PARAMS_UINT(name) uint32_t name;
struct __attribute__((aligned(16))) kk_sampler_params {
   KK_SAMPLER_PARAMS_FIELD_LIST(KK_PARAMS_FLOAT, KK_PARAMS_UINT)
};
#undef KK_PARAMS_FLOAT
#undef KK_PARAMS_UINT

#ifdef __cplusplus
#define KK_PARAMS_ASSERT static_assert
#define KK_PARAMS_ALIGNOF alignof
#else
#define KK_PARAMS_ASSERT _Static_assert
#define KK_PARAMS_ALIGNOF _Alignof
#endif
KK_PARAMS_ASSERT(sizeof(float) == 4, "original parameters require fp32");
KK_PARAMS_ASSERT(sizeof(struct kk_sampler_params) == KK_SAMPLER_PARAMS_STRIDE,
                 "sampler parameter stride");
KK_PARAMS_ASSERT(KK_PARAMS_ALIGNOF(struct kk_sampler_params) == KK_SAMPLER_PARAMS_ALIGNMENT,
                 "sampler parameter alignment");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, min_lod) == 0, "min LOD ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, max_lod) == 4, "max LOD ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, lod_bias) == 8, "LOD bias ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, max_anisotropy) == 12, "anisotropy ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, flags) == 16, "flags ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, compare_op) == 20, "compare operation ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, reduction_mode) == 24, "original reduction ABI");
KK_PARAMS_ASSERT(offsetof(struct kk_sampler_params, reserved2) == 28, "reserved ABI");
KK_PARAMS_ASSERT((KK_SAMPLER_FILTER_VIRTUAL & 0x0007u) == 0,
                 "virtual tag preserves original filters and cube bit2");
KK_PARAMS_ASSERT((KK_SAMPLER_FILTER_VIRTUAL & KK_SAMPLER_FILTER_PARAMS_POINTER) == 0,
                 "pointer presence is independent of virtual sampling");
KK_PARAMS_ASSERT((KK_SAMPLER_FILTER_PARAMS_POINTER & 0xffe7u) == 0,
                 "pointer presence occupies only bit4");
KK_PARAMS_ASSERT(KK_SAMPLER_FILTER_VIRTUAL <= UINT16_MAX, "virtual tag fits metadata");
KK_PARAMS_ASSERT((KK_SAMPLER_PARAMS_MIPMAP_LINEAR & KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE) == 0,
                 "parameter flags are disjoint");
#undef KK_PARAMS_ASSERT
#undef KK_PARAMS_ALIGNOF

/* Emit matching parameter fields and flags into Metal source. */
static inline const char *
kk_sampler_params_msl_declaration(void)
{
#define KK_PARAMS_MSL_FLOAT(name) "    float " #name ";\n"
#define KK_PARAMS_MSL_UINT(name) "    uint " #name ";\n"
#define KK_PARAMS_MSL_FLAG(name, value) "constant uint " #name " = " #value ";\n"
#define KK_PARAMS_STRINGIFY_INNER(value) #value
#define KK_PARAMS_STRINGIFY(value) KK_PARAMS_STRINGIFY_INNER(value)
   return
      "struct alignas(16) kk_sampler_params {\n"
      KK_SAMPLER_PARAMS_FIELD_LIST(KK_PARAMS_MSL_FLOAT, KK_PARAMS_MSL_UINT)
      "};\n"
      KK_SAMPLER_PARAMS_FLAG_LIST(KK_PARAMS_MSL_FLAG)
      "constant ushort KK_SAMPLER_FILTER_VIRTUAL = " KK_PARAMS_STRINGIFY(KK_SAMPLER_FILTER_VIRTUAL) ";\n"
      "constant ushort KK_SAMPLER_FILTER_PARAMS_POINTER = " KK_PARAMS_STRINGIFY(KK_SAMPLER_FILTER_PARAMS_POINTER) ";\n"
      "static_assert(sizeof(kk_sampler_params) == 32, \"sampler parameter stride\");\n"
      "static_assert(alignof(kk_sampler_params) == 16, \"sampler parameter alignment\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, min_lod) == 0, \"min LOD ABI\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, max_lod) == 4, \"max LOD ABI\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, lod_bias) == 8, \"LOD bias ABI\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, max_anisotropy) == 12, \"anisotropy ABI\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, flags) == 16, \"flags ABI\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, compare_op) == 20, \"compare operation ABI\");\n"
      "static_assert(__builtin_offsetof(kk_sampler_params, reduction_mode) == 24, \"original reduction ABI\");\n";
#undef KK_PARAMS_MSL_FLOAT
#undef KK_PARAMS_MSL_UINT
#undef KK_PARAMS_MSL_FLAG
#undef KK_PARAMS_STRINGIFY
#undef KK_PARAMS_STRINGIFY_INNER
}

#endif
