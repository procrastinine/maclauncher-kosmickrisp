// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

// Mesa !45568, Aleksandr Britvin, exact head
// db64cca01164c3fa458dd3ca8e6b28500533ca03.  The separate queue preserves
// Mesa's semaphore ordering while keeping waitForDrawable off the app queue.
export const id = 'metal-wsi-queue';
export function apply({base: sourceRoot, edits}) {
  const edit = (relative, apply) => {
    let source = edits.get(relative) ?? fs.readFileSync(path.join(sourceRoot, relative), 'utf8');
    const replace = (from, to) => {
      if (!source.includes(from)) throw Error(`${relative}: missing WSI anchor ${from.slice(0, 80)}`);
      source = source.replace(from, to);
    };
    apply(replace);
    edits.set(relative, source);
  };
  const kk = (file, apply) => edit('src/kosmickrisp/vulkan/' + file, apply);
  kk('kk_device.h', r => r('   struct kk_queue queue;', `   struct kk_queue queue;
   simple_mtx_t blit_queue_mutex;
   bool has_blit_queue;
   struct kk_queue blit_queue;`));
  kk('kk_device.c', r => {
    r('   if (pCreateInfo->queueCreateInfoCount > 0) {', `   simple_mtx_init(&dev->blit_queue_mutex, mtx_plain);
   if (pCreateInfo->queueCreateInfoCount > 0) {`);
    r('fail_vab_memory:\n', 'fail_vab_memory:\n   simple_mtx_destroy(&dev->blit_queue_mutex);\n');
    r('   /* Meta first since it may destroy Vulkan objects */', `   /* Drain the private present queue before destroying shared meta state. */
   if (dev->has_blit_queue) {
      kk_queue_finish(dev, &dev->blit_queue);
      dev->has_blit_queue = false;
   }
   simple_mtx_destroy(&dev->blit_queue_mutex);

   /* Meta first since it may destroy Vulkan objects */`);
  });
  kk('kk_wsi.c', r => {
    r('static void *\nkk_get_mtl4_command_queue', 'static struct vk_queue *kk_get_blit_queue(VkDevice device);\n\nstatic void *\nkk_get_mtl4_command_queue');
    r(`   VK_FROM_HANDLE(kk_device, dev, device);
   if (dev->has_queue) {
      return dev->queue.mtl_handle;
   }

   return NULL;`, `   struct vk_queue *queue = kk_get_blit_queue(device);
   return queue ? container_of(queue, struct kk_queue, vk)->mtl_handle : NULL;`);
    r('   /* We only have one queue, so just return that one. */\n', '');
    r('   return &dev->queue.vk;', `   simple_mtx_lock(&dev->blit_queue_mutex);
   if (!dev->has_blit_queue) {
      const float priority = 1.0f;
      const VkDeviceQueueCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &priority,
      };
      VkResult result = kk_queue_init(dev, &dev->blit_queue, &info, 0);
      if (result == VK_SUCCESS)
         dev->has_blit_queue = true;
      else
         mesa_logw("kk: failed to create the WSI queue");
   }
   struct vk_queue *queue = dev->has_blit_queue ? &dev->blit_queue.vk : &dev->queue.vk;
   simple_mtx_unlock(&dev->blit_queue_mutex);
   return queue;`);
  });
}
