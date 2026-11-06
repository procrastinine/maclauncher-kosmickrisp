// SPDX-License-Identifier: MIT
export const id = 'additional-graphics-family';

export function apply({edit}) {
  // Keep the original entries, including any sparse flags, in their order.
  // Insert before later family-wide property/flag propagation.
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    /(   pdev->queue_families\[pdev->queue_family_count\+\+\] = \(struct kk_queue_family\)\{\n      \.queue_flags = VK_QUEUE_TRANSFER_BIT[^\n]*,\n      \.queue_count = 1,\n   \};)/,
    original => original + `
   pdev->queue_families[pdev->queue_family_count++] = (struct kk_queue_family){
      .queue_flags =
         VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
      .queue_count = 1,
   };`));

  edit('src/kosmickrisp/vulkan/kk_physical_device.h', s => s.replace(
    '   struct kk_queue_family queue_families[3];',
    '   struct kk_queue_family queue_families[4];'));
  edit('src/kosmickrisp/vulkan/kk_device.h', s => s
    .replace('   /* Two graphics queues and one queue per dedicated family. */',
      '   /* Two queues in family 0 and one in each remaining family. */')
    .replace('   struct kk_queue queues[4];', '   struct kk_queue queues[5];'));
}
