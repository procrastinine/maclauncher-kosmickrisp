// SPDX-License-Identifier: MIT
// The shared input-attachment path already lowers MSAA reads to multisample
// texture loads. Color/input barriers preserve samples by splitting the native
// encoder and storing/reloading attachments before those reads.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,',
    '      .dynamicRenderingLocalReadMultisampledAttachments = true,\n      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,'));
}
