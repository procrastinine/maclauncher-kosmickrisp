// SPDX-License-Identifier: MIT
// Capture requested present images asynchronously
import fs from 'node:fs';
export const id = "capture-requested-present-images-asynchronously";
export const sourceFiles = [
  "kosmickrisp-patches/capture-requested-present-images-asynchronously/src/kosmickrisp/vulkan/kk_frame_capture.c",
  "kosmickrisp-patches/capture-requested-present-images-asynchronously/src/kosmickrisp/vulkan/kk_frame_capture.h"
];
const added = file => fs.readFileSync(new URL("./capture-requested-present-images-asynchronously/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edit("maclauncher/ADAPTATIONS.md", source => source
    .replace("\n## Detailed specifications\n\n* [ATOMIC INT64](reference/KOSMICKRISP_ATOMIC_INT64.md)\n* [ATTACHMENT ACCESS](reference/KOSMICKRISP_ATTACHMENT_ACCESS.md)\n* [CLIP DISTANCES](reference/KOSMICKRISP_CLIP_DISTANCES.md)\n",
      () => "\n## Detailed specifications\n\n### On-demand presentation capture\n\n`KK_CAPTURE_DIR` selects a diagnostic output directory at device creation.\nCreating an empty `request` file in that directory captures layer zero of the\nfirst swapchain in the next available present. One capture is in flight at a\ntime. Rendering semaphores order an image-to-host-buffer copy before ordinary\npresentation. A worker writes `frame-<frame>.ppm` and a one-line\n`frame-<frame>.txt` containing monotonic time in nanoseconds, dimensions, present\nnumber and source format, then removes the request. PPM stores RGB8; higher\nprecision formats are converted to that range. Capture covers the swapchain\nimage before surface scaling or display composition and needs no Screen\nRecording permission. File writing and GPU completion waits run on the worker.\nWith the variable unset, ordinary device dispatch and presentation are unchanged.\n\n* [ATOMIC INT64](reference/KOSMICKRISP_ATOMIC_INT64.md)\n* [ATTACHMENT ACCESS](reference/KOSMICKRISP_ATTACHMENT_ACCESS.md)\n* [CLIP DISTANCES](reference/KOSMICKRISP_CLIP_DISTANCES.md)\n"));
  edit("src/kosmickrisp/vulkan/kk_device.c", source => source
    .replace("\n#include \"kk_device.h\"\n#include \"kk_perf.h\"\n#include \"kk_sampler.h\"\n#include \"kk-sampler-cap-keys.h\"\n#include \"kk_bo.h\"\n",
      () => "\n#include \"kk_device.h\"\n#include \"kk_perf.h\"\n#include \"kk_frame_capture.h\"\n#include \"kk_sampler.h\"\n#include \"kk-sampler-cap-keys.h\"\n#include \"kk_bo.h\"\n")
    .replace("      mtl_start_gpu_capture(dev->mtl_handle, capture_directory);\n   }\n\n   *pDevice = kk_device_to_handle(dev);\n\n   return VK_SUCCESS;\n",
      () => "      mtl_start_gpu_capture(dev->mtl_handle, capture_directory);\n   }\n\n   kk_frame_capture_init(dev);\n   *pDevice = kk_device_to_handle(dev);\n\n   return VK_SUCCESS;\n"));
  edit("src/kosmickrisp/vulkan/kk_device.h", source => source
    .replace("struct kk_buffer;\nstruct kk_bo;\nstruct kk_physical_device;\nstruct vk_pipeline_cache;\nstruct kk_perf;\n\n",
      () => "struct kk_buffer;\nstruct kk_bo;\nstruct kk_physical_device;\nstruct kk_frame_capture;\nstruct vk_pipeline_cache;\nstruct kk_perf;\n\n")
    .replace("   struct kk_bo *interlock_guard;\n   struct kk_interlock_registry *interlock_registry;\n   util_once_flag heap_init_once;\n};\n\nVK_DEFINE_HANDLE_CASTS(kk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)\n",
      () => "   struct kk_bo *interlock_guard;\n   struct kk_interlock_registry *interlock_registry;\n   util_once_flag heap_init_once;\n   struct kk_frame_capture *frame_capture;\n};\n\nVK_DEFINE_HANDLE_CASTS(kk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)\n"));
  edits.set("src/kosmickrisp/vulkan/kk_frame_capture.c", added("src/kosmickrisp/vulkan/kk_frame_capture.c"));
  edits.set("src/kosmickrisp/vulkan/kk_frame_capture.h", added("src/kosmickrisp/vulkan/kk_frame_capture.h"));
  edit("src/kosmickrisp/vulkan/meson.build", source => source
    .replace("  'kk_descriptor_set.h',\n  'kk_descriptor_set.c',\n  'kk_device_lib.c',\n  'kk_device_memory.h',\n  'kk_device_memory.c',\n  'kk_device.h',\n",
      () => "  'kk_descriptor_set.h',\n  'kk_descriptor_set.c',\n  'kk_device_lib.c',\n  'kk_frame_capture.c',\n  'kk_device_memory.h',\n  'kk_device_memory.c',\n  'kk_device.h',\n"));
}
