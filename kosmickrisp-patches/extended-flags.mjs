// SPDX-License-Identifier: MIT
// Mesa's image runtime owns the usage/create/view pNext precedence.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .KHR_maintenance5 = true,',
             '      .KHR_extended_flags = true,\n      .KHR_maintenance5 = true,')
    .replace('      .maintenance5 = true,',
             '      .extendedFlags = true,\n      .maintenance5 = true,'));
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s
    .replaceAll('pImageFormatInfo->flags', 'create_flags')
    .replace('   const VkImageUsageFlags2KHR all_usage =',
             '   const VkImageCreateFlags2KHR create_flags =\n      vk_image_format_info_2_flags(pImageFormatInfo);\n\n   const VkImageUsageFlags2KHR all_usage =')
    .replace('(pCreateInfo->flags & VK_IMAGE_CREATE_DISJOINT_BIT)',
             '(image->vk.create_flags & VK_IMAGE_CREATE_DISJOINT_BIT)')
    .replace('VkImageCreateFlags create_flags,',
             'VkImageCreateFlags2KHR create_flags,'));
  for (const file of ['kk_image_layout.c', 'kk_image_layout.h']) {
    edit('src/kosmickrisp/vulkan/' + file, s => s
      .replaceAll(/\bVkImageUsageFlags\b/g, 'VkImageUsageFlags2KHR')
      .replaceAll(/\bVkImageCreateFlags\b/g, 'VkImageCreateFlags2KHR'));
  }
  edit('src/vulkan/runtime/vk_image.c', s => s
    .replace('if (pCreateInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)',
             'if (create_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)')
    .replace('if (pCreateInfo->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)',
             'if (create_flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)'));
  edit('src/kosmickrisp/vulkan/kk_format.c', s => s.replace(
    '      case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3: {',
    `      case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_4_KHR: {
         /* Flags4 is an additional namespace, not a copy of Flags2.
          * The pinned registry defines no Flags4 bits. */
         VkFormatProperties4KHR *p = ext;
         p->linearTilingFeatures = 0;
         p->optimalTilingFeatures = 0;
         p->bufferFeatures = 0;
         break;
      }

      case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3: {`));
}
