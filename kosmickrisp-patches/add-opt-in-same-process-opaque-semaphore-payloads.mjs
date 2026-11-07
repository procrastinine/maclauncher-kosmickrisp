// SPDX-License-Identifier: MIT
// Add opt-in same-process opaque semaphore payloads
import fs from 'node:fs';
export const id = "add-opt-in-same-process-opaque-semaphore-payloads";
export const sourceFiles = [
  "kosmickrisp-patches/add-opt-in-same-process-opaque-semaphore-payloads/src/kosmickrisp/vulkan/kk_opaque_sync.c",
  "kosmickrisp-patches/add-opt-in-same-process-opaque-semaphore-payloads/src/kosmickrisp/vulkan/kk_opaque_sync.h"
];
const added = file => fs.readFileSync(new URL("./add-opt-in-same-process-opaque-semaphore-payloads/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_debug.c", source => source
    .replace("   {\"custom_border\", KK_EXPERIMENTAL_CUSTOM_BORDER},\n   {\"image_view_min_lod\", KK_EXPERIMENTAL_IMAGE_VIEW_MIN_LOD},\n   {\"optimize_e5b9g9r9\", KK_EXPERIMENTAL_OPTIMIZE_E5B9G9R9},\n   {NULL, 0},\n};\n\n",
      () => "   {\"custom_border\", KK_EXPERIMENTAL_CUSTOM_BORDER},\n   {\"image_view_min_lod\", KK_EXPERIMENTAL_IMAGE_VIEW_MIN_LOD},\n   {\"optimize_e5b9g9r9\", KK_EXPERIMENTAL_OPTIMIZE_E5B9G9R9},\n   {\"opaque_sync\", KK_EXPERIMENTAL_OPAQUE_SYNC},\n   {NULL, 0},\n};\n\n"));
  edit("src/kosmickrisp/vulkan/kk_debug.h", source => source
    .replace("   /* Using allowGPUOptimizedContents for this format causes\n    * vkCmdCopyImage() tests in CTS to fail, but it is otherwise usable */\n   KK_EXPERIMENTAL_OPTIMIZE_E5B9G9R9 = 1ull << 2,\n};\n\nextern enum kk_experimental kk_mesa_experimental_flags;\n",
      () => "   /* Using allowGPUOptimizedContents for this format causes\n    * vkCmdCopyImage() tests in CTS to fail, but it is otherwise usable */\n   KK_EXPERIMENTAL_OPTIMIZE_E5B9G9R9 = 1ull << 2,\n   /* Private same-process opaque semaphore path; properties stay unchanged. */\n   KK_EXPERIMENTAL_OPAQUE_SYNC = 1ull << 3,\n};\n\nextern enum kk_experimental kk_mesa_experimental_flags;\n"));
  edit("src/kosmickrisp/vulkan/kk_device.c", source => source
    .replace("#include \"kk_device.h\"\n#include \"kosmickrisp-rt-commands.h\"\n#include \"kk_perf.h\"\n#include \"kk_frame_capture.h\"\n#include \"util/disk_cache.h\"\n#ifdef ENABLE_SHADER_CACHE\n",
      () => "#include \"kk_device.h\"\n#include \"kosmickrisp-rt-commands.h\"\n#include \"kk_perf.h\"\n#include \"kk_opaque_sync.h\"\n#include \"kk_frame_capture.h\"\n#include \"util/disk_cache.h\"\n#ifdef ENABLE_SHADER_CACHE\n")
    .replace("   /* Metal cannot order a queue behind work that has not been encoded.\n    * Start Mesa's submit worker only when a pending dependency requires it. */\n   vk_device_enable_threaded_submit(&dev->vk);\n\n   /* The original extension has no feature structure. Normalize its\n    * request to the promoted feature used by the shader cache/lowering. */\n",
      () => "   /* Metal cannot order a queue behind work that has not been encoded.\n    * Start Mesa's submit worker only when a pending dependency requires it. */\n   vk_device_enable_threaded_submit(&dev->vk);\n   kk_opaque_sync_init_device(dev);\n\n   /* The original extension has no feature structure. Normalize its\n    * request to the promoted feature used by the shader cache/lowering. */\n"));
  edits.set("src/kosmickrisp/vulkan/kk_opaque_sync.c", added("src/kosmickrisp/vulkan/kk_opaque_sync.c"));
  edits.set("src/kosmickrisp/vulkan/kk_opaque_sync.h", added("src/kosmickrisp/vulkan/kk_opaque_sync.h"));
  edit("src/kosmickrisp/vulkan/kk_queue.c", source => source
    .replace("   mtl_command_queue_add_residency_set(queue->mtl_handle,\n                                       dev->residency_set.handle);\n\n   queue->vk.driver_submit = kk_queue_submit;\n\n   return VK_SUCCESS;\n}\n",
      () => "   mtl_command_queue_add_residency_set(queue->mtl_handle,\n                                       dev->residency_set.handle);\n\n   queue->native_submit = kk_queue_submit;\n   queue->vk.driver_submit = queue->native_submit;\n\n   return VK_SUCCESS;\n}\n"));
  edit("src/kosmickrisp/vulkan/kk_queue.h", source => source
    .replace("struct kk_queue {\n   struct vk_queue vk;\n   struct mtl_command_queue *mtl_handle;\n   struct kk_interlock_queue *interlock_submission;\n\n   /* Resources for command buffer re-recording. These are required for\n",
      () => "struct kk_queue {\n   struct vk_queue vk;\n   struct mtl_command_queue *mtl_handle;\n   /* Immutable setup-selected callback, retained by actual-use adapters. */\n   VkResult (*native_submit)(struct vk_queue *, struct vk_queue_submit *);\n   struct kk_interlock_queue *interlock_submission;\n\n   /* Resources for command buffer re-recording. These are required for\n")
    .replace("\nvoid kk_queue_finish(struct kk_device *dev, struct kk_queue *queue);\n\n/* Register after CPU preflight, before capture. ready is a fresh timeline\n * point, initially zero, completed at value 1. Success transfers one off-thread\n * resume/cancel callback; failure none. All later failures use device loss. */\n",
      () => "\nvoid kk_queue_finish(struct kk_device *dev, struct kk_queue *queue);\n\n\n/* Register after CPU preflight, before capture. ready is a fresh timeline\n * point, initially zero, completed at value 1. Success transfers one off-thread\n * resume/cancel callback; failure none. All later failures use device loss. */\n"));
  edit("src/kosmickrisp/vulkan/meson.build", source => source
    .replace("  'kk_conservative_fs_prolog.c',\n  'kk_sync.h',\n  'kk_sync.c',\n  'kk_wsi.h',\n  'kk_wsi.c',\n)\n",
      () => "  'kk_conservative_fs_prolog.c',\n  'kk_sync.h',\n  'kk_sync.c',\n  'kk_opaque_sync.c',\n  'kk_wsi.h',\n  'kk_wsi.c',\n)\n"));
  edit("src/vulkan/runtime/vk_queue.c", source => source
    .replace("\n      simple_mtx_lock(&real_queue->lock);\n      real_queue->driver_submit_active = submit;\n      result = real_queue->driver_submit(real_queue, submit);\n      real_queue->driver_submit_active = NULL;\n      simple_mtx_unlock(&real_queue->lock);\n   } else {\n      vk_queue_lock(queue);\n      queue->driver_submit_active = submit;\n      result = queue->driver_submit(queue, submit);\n      queue->driver_submit_active = NULL;\n      vk_queue_unlock(queue);\n   }\n",
      () => "\n      simple_mtx_lock(&real_queue->lock);\n      real_queue->driver_submit_active = submit;\n      result = p_atomic_read_relaxed(&real_queue->driver_submit)(\n         real_queue, submit);\n      real_queue->driver_submit_active = NULL;\n      simple_mtx_unlock(&real_queue->lock);\n   } else {\n      vk_queue_lock(queue);\n      queue->driver_submit_active = submit;\n      result = p_atomic_read_relaxed(&queue->driver_submit)(queue, submit);\n      queue->driver_submit_active = NULL;\n      vk_queue_unlock(queue);\n   }\n"));
}
