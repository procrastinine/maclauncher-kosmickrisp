// SPDX-License-Identifier: MIT
export const id = 'image-compression-control';
export function apply({edit}) {
  const change = (file, from, to) => edit('src/kosmickrisp/' + file, (_s, replace) => replace(from, to));
  change('vulkan/kk_physical_device.c',
    '   kk_get_device_extensions(instance, pdev, &supported_extensions);',
    `   kk_get_device_extensions(instance, pdev, &supported_extensions);
   supported_extensions.EXT_image_compression_control = true;`);
  change('vulkan/kk_physical_device.c',
    '   kk_get_device_features(pdev, &supported_extensions, &supported_features);',
    `   kk_get_device_features(pdev, &supported_extensions, &supported_features);
   supported_features.imageCompressionControl = true;`);
  change('vulkan/kk_image_layout.c',
    `      initial_layout);`,
    `      initial_layout) &&
      !(image->compr_flags & VK_IMAGE_COMPRESSION_DISABLED_EXT);`);
  change('vulkan/kk_image.c',
    '   const unsigned plane_count =\n      vk_format_get_plane_count(pImageFormatInfo->format);',
    `   /* Metal exposes lossless/default or disabled compression, not an
    * explicit fixed-rate format. Unsupported fixed-rate hints retain the
    * default lossless policy, as allowed by the compression-control contract. */
   const VkImageCompressionControlEXT *compression = vk_find_struct_const(
      pImageFormatInfo->pNext, IMAGE_COMPRESSION_CONTROL_EXT);
   bool compression_disabled = compression &&
      (compression->flags & VK_IMAGE_COMPRESSION_DISABLED_EXT);
   VkImageCompressionPropertiesEXT *compression_props = vk_find_struct(
      pImageFormatProperties->pNext, IMAGE_COMPRESSION_PROPERTIES_EXT);
   if (compression_props) {
      bool optimize = !compression_disabled && kk_image_layout_can_optimize(
         pdev, all_usage, pImageFormatInfo->tiling, pImageFormatInfo->flags,
         p_format);
      compression_props->imageCompressionFlags = optimize
         ? VK_IMAGE_COMPRESSION_DEFAULT_EXT : VK_IMAGE_COMPRESSION_DISABLED_EXT;
      compression_props->imageCompressionFixedRateFlags =
         VK_IMAGE_COMPRESSION_FIXED_RATE_NONE_EXT;
   }

   const unsigned plane_count =
      vk_format_get_plane_count(pImageFormatInfo->format);`);
  change('vulkan/kk_image.c',
    '         host_props->identicalMemoryLayout = host_props->optimalDeviceAccess;',
    `         host_props->identicalMemoryLayout = host_props->optimalDeviceAccess;
         if (compression_disabled) {
            host_props->optimalDeviceAccess = true;
            host_props->identicalMemoryLayout = true;
         }`);
  change('vulkan/kk_image.c',
    '   VkSubresourceHostMemcpySize *memcpy_size =',
    `   VkImageCompressionPropertiesEXT *compression_props =
      vk_find_struct(pLayout->pNext, IMAGE_COMPRESSION_PROPERTIES_EXT);
   if (compression_props) {
      compression_props->imageCompressionFlags = plane->layout.optimized_layout
         ? VK_IMAGE_COMPRESSION_DEFAULT_EXT : VK_IMAGE_COMPRESSION_DISABLED_EXT;
      compression_props->imageCompressionFixedRateFlags =
         VK_IMAGE_COMPRESSION_FIXED_RATE_NONE_EXT;
   }

   VkSubresourceHostMemcpySize *memcpy_size =`);
}
