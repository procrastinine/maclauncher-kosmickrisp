// SPDX-License-Identifier: MIT
/** A shared compiler-assigned feature bit layout for shader compilation/cache keys. */
export const id = 'shader-feature-key';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.h', (s, replace) => replace(
    'struct kk_cmd_buffer;',
    `/* Feature enumerators are bit indices, assigned once by the compiler.
 * New shader-affecting features belong here without explicit values. */
enum kk_feature_bit {
   KK_FEAT_CUSTOM_BORDER,
   KK_FEAT_NULL_DESCRIPTOR,
   KK_FEAT_IMAGE_VIEW_MIN_LOD,
   KK_FEAT_PIPELINE_STATISTICS,
   KK_FEAT_SAMPLER_MINMAX,
   KK_FEAT_COUNT,
};

typedef uint32_t kk_feature_key;
static_assert(KK_FEAT_COUNT <= sizeof(kk_feature_key) * CHAR_BIT,
              "shader feature bits fit the shader-cache key");

struct kk_cmd_buffer;`).replace(
    '#include "kk_private.h"', '#include "kk_private.h"\n#include <limits.h>'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', (s, replace) => {
    s = replace(`enum kk_feature_key {
   KK_FEAT_CUSTOM_BORDER = BITFIELD_BIT(0),
   KK_FEAT_NULL_DESCRIPTOR = BITFIELD_BIT(1),
   KK_FEAT_IMAGE_VIEW_MIN_LOD = BITFIELD_BIT(2),
};

`, '');
    s = s.replaceAll('enum kk_feature_key', 'kk_feature_key');
    for (const feature of ['CUSTOM_BORDER', 'NULL_DESCRIPTOR', 'IMAGE_VIEW_MIN_LOD'])
      s = s.replaceAll('KK_FEAT_' + feature, 'BITFIELD_BIT(KK_FEAT_' + feature + ')');
    // Including the enum's count separates this layout from old ambiguous
    // cache entries and automatically changes the namespace as features grow.
    return s.replace(
      '   kk_feature_key feature_key = kk_make_feature_key(enabled_features);',
      `   const uint32_t feature_key[] = {
      KK_FEAT_COUNT, kk_make_feature_key(enabled_features),
   };`);
  });
}
