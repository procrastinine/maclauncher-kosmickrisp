// SPDX-License-Identifier: MIT
// Guard destruction of the lazy implicit cache
export const id = "guard-destruction-of-the-lazy-implicit-cache";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_device.c", source => source
    .replace("fail_meta:\n   kk_device_finish_meta(dev);\nfail_mem_cache:\n   vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);\n   simple_mtx_destroy(&dev->pipeline_cache_init_mutex);\n   while (dev->queue_count > 0)\n      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);\n",
      () => "fail_meta:\n   kk_device_finish_meta(dev);\nfail_mem_cache:\n   if (dev->vk.mem_cache)\n      vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);\n   simple_mtx_destroy(&dev->pipeline_cache_init_mutex);\n   while (dev->queue_count > 0)\n      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);\n")
    .replace("   mtl_release(dev->residency_set.handle);\n   simple_mtx_destroy(&dev->residency_set.mutex);\n\n   vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);\n   simple_mtx_destroy(&dev->pipeline_cache_init_mutex);\n   kk_release_compiler(dev);\n   kk_interlock_submission_finish(dev);\n",
      () => "   mtl_release(dev->residency_set.handle);\n   simple_mtx_destroy(&dev->residency_set.mutex);\n\n   if (dev->vk.mem_cache)\n      vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);\n   simple_mtx_destroy(&dev->pipeline_cache_init_mutex);\n   kk_release_compiler(dev);\n   kk_interlock_submission_finish(dev);\n"));
}
