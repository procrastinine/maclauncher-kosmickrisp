// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const id = 'sampler-minmax';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-sampler-minmax.h'];

export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_descriptor_types.h', (s, replace) => replace(
    '   uint16_t sampler_pad[3];',
    `   uint16_t sampler_reduction_mode;
   uint16_t sampler_filter_bits;
   uint16_t sampler_address_modes;`).replace(
    'static_assert(sizeof(struct kk_sampled_image_descriptor)',
    `static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_reduction_mode) == 42,
              "sampler reduction occupies descriptor padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits) == 44,
              "sampler filters occupy descriptor padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_address_modes) == 46,
              "sampler addressing occupies descriptor padding");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_mipmap_mode) == 22,
              "mipmap metadata ABI");
static_assert(offsetof(struct kk_sampled_image_descriptor, sampler_query_lod_bias_fp16) == 40,
              "sampler bias metadata ABI");
static_assert(sizeof(struct kk_sampled_image_descriptor)`));
  edit('src/kosmickrisp/vulkan/kk_sampler.h', (s, replace) => replace(
    '   uint16_t lod_max_fp16;',
    `   uint16_t lod_max_fp16;
   uint16_t reduction_mode;
   uint16_t filter_bits;
   uint16_t address_modes;`));
  edit('src/kosmickrisp/vulkan/kk_sampler.c', (s, replace) => {
    // Apple10 retains native reduction. Earlier GPUs use nearest, explicit-LOD
    // taps; the requested filter, bias and clamps remain in the descriptor.
    s = replace('   if (pdev->vk.supported_extensions.EXT_sampler_filter_minmax) {',
                '   if (pdev->info.gpu_apple_family >= 10) {');
    s = s.replace('   if (pdev->vk.supported_extensions.EXT_sampler_filter_minmax)\n',
                  '   if (pdev->info.gpu_apple_family >= 10)\n');
    s = s.replace('   struct mtl_sampler_packed packed =\n',
      `   sampler->reduction_mode = pdev->info.gpu_apple_family < 10
                                  ? sampler->vk.reduction_mode : 0;
   sampler->filter_bits = (pCreateInfo->minFilter == VK_FILTER_LINEAR ? 1 : 0) |
                          (pCreateInfo->magFilter == VK_FILTER_LINEAR ? 2 : 0);
   sampler->address_modes = pCreateInfo->addressModeU |
      (pCreateInfo->addressModeV << 3) | (pCreateInfo->addressModeW << 6) |
      (pCreateInfo->unnormalizedCoordinates ? 1u << 9 : 0);

   struct mtl_sampler_packed packed =
`);
    s = s.replace('   result = kk_sampler_heap_add(dev, packed, &sampler->planes[0].hw);',
      `   if (sampler->reduction_mode) {
      packed.min_filter = packed.mag_filter = MTL_SAMPLER_MIN_MAG_FILTER_NEAREST;
      packed.mip_filter = pCreateInfo->unnormalizedCoordinates
                             ? MTL_SAMPLER_MIP_FILTER_NOT_MIP_MAPPED
                             : MTL_SAMPLER_MIP_FILTER_NEAREST;
      packed.min_lod = 0;
      packed.max_lod = VK_LOD_CLAMP_NONE;
      packed.lod_bias = 0;
      packed.max_anisotropy = 1;
   }
   result = kk_sampler_heap_add(dev, packed, &sampler->planes[0].hw);`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', (s, replace) => replace(
    '         desc[plane].sampler_lod_max_fp16 = sampler->lod_max_fp16;',
    `         desc[plane].sampler_lod_max_fp16 = sampler->lod_max_fp16;
         desc[plane].sampler_reduction_mode = sampler->reduction_mode;
         desc[plane].sampler_filter_bits = sampler->filter_bits;
         desc[plane].sampler_address_modes = sampler->address_modes;`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', (s, replace) => replace(
    'static bool\nlower_tex(',
    '#include "kk_sampler_minmax.h"\n\nstatic bool\nlower_tex('));
  edit('src/kosmickrisp/vulkan/kk_shader.h', (s, replace) => replace(
    'bool\nkk_nir_lower_descriptors(',
    `bool kk_nir_lower_sampler_minmax(nir_shader *nir,
      uint32_t set_layout_count,
      struct vk_descriptor_set_layout *const *set_layouts);

bool
kk_nir_lower_descriptors(`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', (s, replace) => {
    s = s.replace('   if (feats->minLod)\n',
      '   if (feats->samplerFilterMinmax)\n      key |= BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX);\n   if (feats->minLod)\n');
    s = s.replace('   if (features & BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD))\n      NIR_PASS(_, nir, kk_nir_lower_image_view_min_lod);\n', '');
    return s.replace('   if (features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER))\n',
      `   if (features & BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD))
      NIR_PASS(_, nir, kk_nir_lower_image_view_min_lod);

   if ((features & BITFIELD_BIT(KK_FEAT_SAMPLER_MINMAX)) && pdev->info.gpu_apple_family < 10)
      NIR_PASS(_, nir, kk_nir_lower_sampler_minmax, set_layout_count, set_layouts);

   if (features & BITFIELD_BIT(KK_FEAT_CUSTOM_BORDER))
`);
  });
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', (s, replace) => replace(
    '.EXT_sampler_filter_minmax = pdev->info.gpu_apple_family >= 10,',
    '.EXT_sampler_filter_minmax = true,'));
  edit('src/kosmickrisp/vulkan/kk_device.c', (s, replace) => replace(
    '   dev->vk.shader_ops = &kk_device_shader_ops;',
    `   /* The original extension has no feature structure. Normalize its
    * request to the promoted feature used by the shader cache/lowering. */
   dev->vk.enabled_features.samplerFilterMinmax |=
      dev->vk.enabled_extensions.EXT_sampler_filter_minmax;

   dev->vk.shader_ops = &kk_device_shader_ops;`));
  edit('src/kosmickrisp/vulkan/kk_image.c', (s, replace) => replace(
    '      if (pdev->vk.supported_extensions.EXT_sampler_filter_minmax)\n         features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT;',
    '').replace('   /* TODO: VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT */',
    `   /* Centered nearest taps preserve every sampleable format, including
    * integer, normalized, sRGB, compressed and depth values. Native Metal
    * reduction retains its format-filter capability boundary. */
   if (pdev->vk.supported_extensions.EXT_sampler_filter_minmax &&
       (pdev->info.gpu_apple_family < 10 || va_format->filter))
      features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT;

   /* TODO: VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT */`));
  edits.set('src/kosmickrisp/vulkan/kk_sampler_minmax.h',
    fs.readFileSync(new URL('./kosmickrisp-sampler-minmax.h', import.meta.url), 'utf8'));
}
