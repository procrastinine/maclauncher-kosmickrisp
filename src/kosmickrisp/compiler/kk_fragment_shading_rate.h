/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_H
#define KK_FRAGMENT_SHADING_RATE_H
#include "nir.h"
struct nir_builder;
/* Rates use (log2 width << 2) | log2 height; extents are pixel counts. */
nir_def *kk_nir_vrs_decode_extent(struct nir_builder *, nir_def *rate);
/* KEEP/REPLACE/MIN/MAX/MUL on extents; MUL is the strict product. */
nir_def *kk_nir_vrs_combine_extent(struct nir_builder *, nir_def *a, nir_def *c, nir_def *op);
/* A shader that runs at 1x1 reads the 1x1 rate. */
void kk_nir_vrs_lower_fine_rate(nir_shader *nir);
#endif
