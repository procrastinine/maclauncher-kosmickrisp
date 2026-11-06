// SPDX-License-Identifier: MIT
// Views use the native Metal representation and the same channel composition
// as existing packed formats. Non-native layouts are sampled only.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_format.c', s => {
    s = s.replace('   /* MTL_FMT_NATIVE(A8_UNORM, MTL_FMT_ALL_NO_ATOMIC(8), MTL_FMT_TB_ALL), */',
      '   MTL_FMT_NATIVE(A8_UNORM, MTL_FMT_FS(8), MTL_FMT_TB_NONE),');
    s = s.replace('   /* MTL_FMT_NATIVE(A1B5G5R5_UNORM, MTL_FMT_FCBMRS(16), MTL_FMT_TB_NONE), */',
      `   MTL_FMT_NATIVE(A1B5G5R5_UNORM, MTL_FMT_FCBMRS(16), MTL_FMT_TB_NONE),
   MTL_FMT(R5G6B5_UNORM, B5G6R5_UNORM, MTL_SWIZZLE_BGRA, MTL_FMT_FS(16), MTL_FMT_TB_NONE, false),
   MTL_FMT(A1R5G5B5_UNORM, A1B5G5R5_UNORM, MTL_SWIZZLE_BGRA, MTL_FMT_FS(16), MTL_FMT_TB_NONE, false),
   MTL_FMT(R5G5B5A1_UNORM, B5G5R5A1_UNORM, MTL_SWIZZLE_BGRA, MTL_FMT_FS(16), MTL_FMT_TB_NONE, false),`);
    s = s.replace('   // 64-bit formats',
      `   MTL_FMT(B8G8R8A8_SNORM, R8G8B8A8_SNORM, MTL_SWIZZLE_BGRA, MTL_FMT_FS(32), MTL_FMT_TB_NONE, false),
   MTL_FMT(B8G8R8A8_UINT, R8G8B8A8_UINT, MTL_SWIZZLE_BGRA, MTL_FMT_SAMPLE(32), MTL_FMT_TB_NONE, false),
   MTL_FMT(B8G8R8A8_SINT, R8G8B8A8_SINT, MTL_SWIZZLE_BGRA, MTL_FMT_SAMPLE(32), MTL_FMT_TB_NONE, false),

   // 64-bit formats`);
    s = s.replaceAll(`   /* Hardware has issues with border color opaque black, and since it's not
    * required by Vulkan, we can just disable it.
    */
`, '');
    return s.replace('// Filter, Sparse',
      `#define MTL_FMT_SAMPLE(width) \\
   .bit_widths = width, .filter = 0u, .write = 0u, .color = 0u, .blend = 0u, \\
   .msaa = 0u, .resolve = 0u, .sparse = 0u, .atomic = 0u

// Filter, Sparse`);
  });
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '   if (features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER))\n      NIR_PASS(_, nir, kk_nir_lower_custom_border);',
    '   /* Fixed opaque-black borders also use this shared lowering. */\n   NIR_PASS(_, nir, kk_nir_lower_custom_border);'));
  edit('src/kosmickrisp/vulkan/kk_sampler.c', s => s.replace(
    '   bool custom_border = dev->vk.enabled_features.customBorderColors &&',
    `   /* Opaque black is a core mode. A8 and packed formats need this
    * correction even without the optional custom-border feature enabled. */
   bool custom_border = (dev->vk.enabled_features.customBorderColors ||
                         pCreateInfo->borderColor == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) &&`));
}
