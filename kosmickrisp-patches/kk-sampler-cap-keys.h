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
