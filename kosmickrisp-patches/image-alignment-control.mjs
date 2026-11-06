// SPDX-License-Identifier: MIT
// The existing heapTextureSizeAndAlign query chooses Metal's minimum viable
// layout, including its compression metadata. It already satisfies each
// maximum-alignment hint whenever such a layout is possible; no padding or
// compression-disabling fallback is introduced just to match a hint.
export const id = 'image-alignment-control';
export function apply({edit}) {
  const file = 'src/kosmickrisp/vulkan/kk_physical_device.c';
  edit(file, (s, replace) => replace(
    '   kk_get_device_extensions(instance, pdev, &supported_extensions);',
    `   kk_get_device_extensions(instance, pdev, &supported_extensions);
   supported_extensions.MESA_image_alignment_control = true;`));
  edit(file, (s, replace) => replace(
    '   kk_get_device_features(pdev, &supported_extensions, &supported_features);',
    `   kk_get_device_features(pdev, &supported_extensions, &supported_features);
   supported_features.imageAlignmentControl = true;`));
  edit(file, (s, replace) => replace(
    '   kk_get_device_properties(pdev, instance, &supported_extensions, &properties);',
    `   kk_get_device_properties(pdev, instance, &supported_extensions, &properties);
   properties.supportedImageAlignmentMask = 4096 | 16384 | 65536;`));
}
