// SPDX-License-Identifier: MIT
// Destroy the sync publisher mutex when native event creation fails
export const id = "destroy-the-sync-publisher-mutex-when-native-event";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_sync.c", source => source
    .replace("   if (mtx_init(&timeline->publish_mutex, mtx_plain) != thrd_success)\n      return VK_ERROR_OUT_OF_HOST_MEMORY;\n   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);\n   if (!timeline->mtl_handle)\n      return VK_ERROR_OUT_OF_DEVICE_MEMORY;\n   if (!timeline->mtl_handle) {\n      mtx_destroy(&timeline->publish_mutex);\n      return VK_ERROR_OUT_OF_DEVICE_MEMORY;\n",
      () => "   if (mtx_init(&timeline->publish_mutex, mtx_plain) != thrd_success)\n      return VK_ERROR_OUT_OF_HOST_MEMORY;\n   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);\n   if (!timeline->mtl_handle) {\n      mtx_destroy(&timeline->publish_mutex);\n      return VK_ERROR_OUT_OF_DEVICE_MEMORY;\n"));
}
