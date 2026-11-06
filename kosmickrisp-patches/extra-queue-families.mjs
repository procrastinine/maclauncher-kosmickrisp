// SPDX-License-Identifier: MIT
export const id = 'extra-queue-families';

export function apply({edit}) {
  // Append after the existing family, retaining its flags (including sparse).
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    /(   pdev->queue_families\[pdev->queue_family_count\+\+\] = \(struct kk_queue_family\)\{[\s\S]*?      \.queue_count = )1(,\n   \};)/,
    (_match, head, tail) => head + '2' + tail + `
   pdev->queue_families[pdev->queue_family_count++] = (struct kk_queue_family){
      .queue_flags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
      .queue_count = 1,
   };
   pdev->queue_families[pdev->queue_family_count++] = (struct kk_queue_family){
      .queue_flags = VK_QUEUE_TRANSFER_BIT,
      .queue_count = 1,
   };`));

  edit('src/kosmickrisp/vulkan/kk_device.h', s => s.replace(
    '   bool has_queue;\n   struct kk_queue queue;',
    `   uint32_t queue_count;
   /* Two graphics queues and one queue per dedicated family. */
   struct kk_queue queues[4];`));

  edit('src/kosmickrisp/vulkan/kk_device.c', s => s.replace(
    `   if (pCreateInfo->queueCreateInfoCount > 0) {
      result =
         kk_queue_init(dev, &dev->queue, &pCreateInfo->pQueueCreateInfos[0], 0);
      if (result != VK_SUCCESS)
         goto fail_vab_memory;
      dev->has_queue = true;
   }`,
    `   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
      const VkDeviceQueueCreateInfo *info = &pCreateInfo->pQueueCreateInfos[i];
      for (uint32_t q = 0; q < info->queueCount; ++q) {
         assert(dev->queue_count < ARRAY_SIZE(dev->queues));
         result = kk_queue_init(dev, &dev->queues[dev->queue_count], info, q);
         if (result != VK_SUCCESS)
            goto fail_mem_cache;
         ++dev->queue_count;
      }
   }`).replaceAll(
    `   if (dev->has_queue) {
      kk_queue_finish(dev, &dev->queue);
      dev->has_queue = false;
   }`,
    `   while (dev->queue_count > 0)
      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);`));

  // The WSI module owns a separate, lazily created queue. Only its fallback
  // needs the first application queue; it is independent of family numbering.
  edit('src/kosmickrisp/vulkan/kk_wsi.c', s => s
    .replace('if (!dev->has_queue)', 'if (dev->queue_count == 0)')
    .replace('&dev->queue.vk', '&dev->queues[0].vk'));

  edit('src/kosmickrisp/vulkan/kk_queue.c', s => s.replace(
    '   queue->mtl_handle = mtl_new_command_queue(dev->mtl_handle);',
    `   queue->mtl_handle = mtl_new_command_queue(dev->mtl_handle);
   if (!queue->mtl_handle) {
      cnd_destroy(&queue->cond);
      mtx_destroy(&queue->mutex);
      util_dynarray_fini(&queue->free_cmd_buffers);
      kk_DestroyCommandPool(kk_device_to_handle(dev), pool_handle, NULL);
      vk_queue_finish(&queue->vk);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }`));
}
