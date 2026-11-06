// SPDX-License-Identifier: MIT
// Integer resolves use Mesa's sample-zero meta path, including partial render
// areas, array layers and explicit CmdResolveImage2. Metal cannot average them.
export function apply({edit}) {
  edit('src/kosmickrisp/bridge/mtl_device.h', s => s.replace(
    'bool mtl_device_supports_sample_count(',
    'bool mtl_device_supports_32bit_msaa(mtl_device *dev);\nbool mtl_device_supports_sample_count('));
  edit('src/kosmickrisp/bridge/mtl_device.m', s => s.replace(
    'bool\nmtl_device_supports_sample_count(',
    `bool
mtl_device_supports_32bit_msaa(mtl_device *dev)
{
   return [(id<MTLDevice>)dev supports32BitMSAA];
}

bool
mtl_device_supports_sample_count(`));
  edit('src/kosmickrisp/vulkan/kk_physical_device.h', s => s.replace(
    '   VkSampleCountFlags supported_sample_counts;',
    '   VkSampleCountFlags supported_sample_counts;\n   bool supports_32bit_msaa;'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '   pdev->info.supported_sample_counts = VK_SAMPLE_COUNT_1_BIT;',
    `   pdev->info.supports_32bit_msaa =
      mtl_device_supports_32bit_msaa(pdev->mtl_dev_handle);
   pdev->info.supported_sample_counts = VK_SAMPLE_COUNT_1_BIT;`));
  edit('src/kosmickrisp/vulkan/kk_format.c', s => {
    s = s.replace('MTL_FMT_NATIVE(R32_UINT, MTL_FMT_WCSA(32)',
                  'MTL_FMT_NATIVE(R32_UINT, MTL_FMT_WCMSA(32)')
         .replace('MTL_FMT_NATIVE(R32_SINT, MTL_FMT_WCSA(32)',
                  'MTL_FMT_NATIVE(R32_SINT, MTL_FMT_WCMSA(32)')
         .replace('MTL_FMT_NATIVE(R32G32B32A32_UINT, MTL_FMT_WCS(128)',
                  'MTL_FMT_NATIVE(R32G32B32A32_UINT, MTL_FMT_WCMS(128)')
         .replace('MTL_FMT_NATIVE(R32G32B32A32_SINT, MTL_FMT_WCS(128)',
                  'MTL_FMT_NATIVE(R32G32B32A32_SINT, MTL_FMT_WCMS(128)');
    return s.replace('// Write, Color, Sparse, Atomic',
      `#define MTL_FMT_WCMSA(width) \\
   .bit_widths = width, .filter = 0u, .write = 1u, .color = 1u, .blend = 0u, \\
   .msaa = 1u, .resolve = 0u, .sparse = 1u, .atomic = 1u

// Write, Color, Sparse, Atomic`);
  });
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s.replace(
    '      sampleCounts = pdev->info.supported_sample_counts;',
    `      enum pipe_format format = vk_format_to_pipe_format(pImageFormatInfo->format);
      const struct kk_va_format *caps = kk_get_va_format(format);
      bool integer32 = util_format_is_pure_integer(format) &&
                       util_format_get_component_bits(format, UTIL_FORMAT_COLORSPACE_RGB, 0) == 32;
      if (caps->msaa && (!integer32 || pdev->info.supports_32bit_msaa))
         sampleCounts = pdev->info.supported_sample_counts;`));
}
