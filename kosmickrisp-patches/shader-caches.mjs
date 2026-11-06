// SPDX-License-Identifier: MIT
// Cache initialization follows Mesa NVK and HoneyKrisp (Mesa4f071f891d6339).
export const id = 'shader-caches';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_device.c', s => s.replace(
    '   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {',
    `   /* Weak entries reuse live shaders and their native Metal pipelines. */
   struct vk_pipeline_cache_create_info cache_info = {.weak_ref = true};
   dev->vk.mem_cache = vk_pipeline_cache_create(&dev->vk, &cache_info, NULL);
   if (dev->vk.mem_cache == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_mem_cache;
   }

   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {`).replace(
    'fail_mem_cache:\n',
    'fail_mem_cache:\n   vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);\n').replace(
    '   kk_release_compiler(dev);\n\n   vk_device_finish(&dev->vk);',
    '   vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);\n   kk_release_compiler(dev);\n\n   vk_device_finish(&dev->vk);'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '   memcpy(pdev->vk.properties.shaderBinaryUUID, blake3, VK_UUID_SIZE);',
    `   memcpy(pdev->vk.properties.shaderBinaryUUID, blake3, VK_UUID_SIZE);

#ifdef ENABLE_SHADER_CACHE
   char renderer[32];
   snprintf(renderer, sizeof(renderer), "kosmickrisp_apple_%u", pdev->info.gpu_apple_family);
   char timestamp[BLAKE3_HEX_LEN];
   /* driver_build_sha is a16-byte UUID; format its full BLAKE3 digest. */
   _mesa_blake3_format(timestamp, blake3);
   pdev->vk.disk_cache = disk_cache_create(renderer, timestamp,
                                          pdev->settings.disabled_workarounds);
#endif`));
}
