/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_CAPACITY_KEYS_H
#define KK_SAMPLER_CAPACITY_KEYS_H
#include <string.h>
#define KK_SAMPLER_CAP_CANONICAL_MAX 512u

static inline void
kk_sampler_cap_key_normalize(struct mtl_sampler_packed *out,
                             const struct mtl_sampler_packed *in)
{
   const struct mtl_sampler_packed value = *in;
   memset(out, 0, sizeof(*out));
#define COPY(F) out->F = value.F
   COPY(mode_u); COPY(mode_v); COPY(mode_w); COPY(border_color);
   COPY(min_filter); COPY(mag_filter); COPY(mip_filter); COPY(reduction_mode);
   COPY(compare_func); COPY(lod_bias); COPY(min_lod); COPY(max_lod);
   COPY(max_anisotropy); COPY(normalized_coordinates);
#undef COPY
}

static inline void
kk_sampler_cap_key(struct mtl_sampler_packed *out,
                    const struct mtl_sampler_packed *in)
{
   kk_sampler_cap_key_normalize(out, in);
   out->min_filter = out->mag_filter = MTL_SAMPLER_MIN_MAG_FILTER_NEAREST;
   out->mip_filter = out->normalized_coordinates ? MTL_SAMPLER_MIP_FILTER_NEAREST
                                                : MTL_SAMPLER_MIP_FILTER_NOT_MIP_MAPPED;
   out->reduction_mode = MTL_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
   out->lod_bias = out->min_lod = 0.0f;
   out->max_lod = 1000.0f;
   out->max_anisotropy = 1;
   if (!out->normalized_coordinates)
      out->mode_w = MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   if (out->compare_func == MTL_COMPARE_FUNCTION_NEVER) {
      out->mode_u = out->mode_v = out->mode_w = MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      out->border_color = MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE;
   } else if (out->compare_func != MTL_COMPARE_FUNCTION_ALWAYS) {
      out->mode_w = MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      if (out->border_color == MTL_SAMPLER_BORDER_COLOR_OPAQUE_BLACK)
         out->border_color = MTL_SAMPLER_BORDER_COLOR_TRANSPARENT_BLACK;
   }
   if (out->mode_u != MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR &&
       out->mode_v != MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR &&
       out->mode_w != MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR)
      out->border_color = MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE;
}

/* Enumerate each legal point state once, in the original reservation order.
 * Comparison collapses W and opaque black; no-border modes collapse color.
 * Keep those equivalences here rather than constructing duplicate states. */
static inline uint32_t
kk_sampler_cap_canonical_keys(struct mtl_sampler_packed *keys)
{
   const enum mtl_sampler_address_mode modes[] = {
      MTL_SAMPLER_ADDRESS_MODE_REPEAT, MTL_SAMPLER_ADDRESS_MODE_MIRROR_REPEAT,
      MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR,
      MTL_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
   };
   struct mtl_sampler_packed key;
   memset(&key, 0, sizeof(key));
   key.min_filter = key.mag_filter = MTL_SAMPLER_MIN_MAG_FILTER_NEAREST;
   key.reduction_mode = MTL_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
   key.max_lod = 1000.0f;
   key.max_anisotropy = 1;
   uint32_t count = 0;
   for (unsigned normalized = 0; normalized < 2; ++normalized) {
      key.normalized_coordinates = normalized;
      key.mip_filter = normalized ? MTL_SAMPLER_MIP_FILTER_NEAREST :
                                   MTL_SAMPLER_MIP_FILTER_NOT_MIP_MAPPED;
      for (unsigned compare = normalized ? 0 : MTL_COMPARE_FUNCTION_ALWAYS;
           compare <= MTL_COMPARE_FUNCTION_ALWAYS; ++compare) {
         key.compare_func = compare;
         if (compare == MTL_COMPARE_FUNCTION_NEVER) {
            key.mode_u = key.mode_v = key.mode_w = MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            key.border_color = MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE;
            memcpy(&keys[count++], &key, sizeof(key));
            continue;
         }
         for (unsigned u = normalized ? 0 : 2; u < (normalized ? 5 : 4); ++u)
         for (unsigned v = normalized ? 0 : 2; v < (normalized ? 5 : 4); ++v)
         for (unsigned w = 0; w < (normalized && compare == MTL_COMPARE_FUNCTION_ALWAYS ? 5 : 1); ++w) {
            key.mode_u = modes[u]; key.mode_v = modes[v];
            key.mode_w = normalized && compare == MTL_COMPARE_FUNCTION_ALWAYS ?
                            modes[w] : MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            bool border = key.mode_u == MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR ||
                          key.mode_v == MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR ||
                          key.mode_w == MTL_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER_COLOR;
            for (unsigned color = border ? 0 : MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE;
                 color <= MTL_SAMPLER_BORDER_COLOR_OPAQUE_WHITE; ++color) {
               if (compare != MTL_COMPARE_FUNCTION_ALWAYS &&
                   color == MTL_SAMPLER_BORDER_COLOR_OPAQUE_BLACK)
                  continue;
               key.border_color = color;
               assert(count < KK_SAMPLER_CAP_CANONICAL_MAX);
               memcpy(&keys[count++], &key, sizeof(key));
            }
         }
      }
   }
   return count;
}

static inline bool
kk_sampler_cap_keys_equal(const struct mtl_sampler_packed *a,
                           const struct mtl_sampler_packed *b)
{
   struct mtl_sampler_packed x, y;
   kk_sampler_cap_key_normalize(&x, a);
   kk_sampler_cap_key_normalize(&y, b);
   return memcmp(&x, &y, sizeof(x)) == 0;
}
#endif
