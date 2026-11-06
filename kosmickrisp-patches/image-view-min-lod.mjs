// SPDX-License-Identifier: MIT
export const id = 'image-view-min-lod';
export function apply({edit}) {
  // Mesa's existing feature key gates its image-view min-LOD lowering.
  edit('src/kosmickrisp/vulkan/kk_debug.c', s => s.replace(
    'experimental_flags, KK_EXPERIMENTAL_CUSTOM_BORDER);',
    'experimental_flags, KK_EXPERIMENTAL_CUSTOM_BORDER | KK_EXPERIMENTAL_IMAGE_VIEW_MIN_LOD);'));
  edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', s => s
    .replace('#include "kk_private.h"', '#include "kk_private.h"\n#include <stddef.h>')
    .replace('"kk_sampled_image_descriptor has no holes");',
      `"kk_sampled_image_descriptor has no holes");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) ==
              offsetof(struct kk_sampled_image_descriptor, sampler_lod_max_fp16) +
              sizeof(uint16_t), "image min LOD follows sampler LOD");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) ==
              offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) +
              sizeof(uint16_t), "image min LOD fields are adjacent");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) +
              sizeof(uint16_t) <=
              offsetof(struct kk_sampled_image_descriptor, border),
              "image min LOD does not overlap trailing descriptor fields");`));
}
