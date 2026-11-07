// SPDX-License-Identifier: MIT
// Retain optional command-queue fault reports
import fs from 'node:fs';
export const id = "retain-optional-command-queue-fault-reports";
export const sourceFiles = [
  "kosmickrisp-patches/retain-optional-command-queue-fault-reports/src/kosmickrisp/vulkan/kk_device_fault.c",
  "kosmickrisp-patches/retain-optional-command-queue-fault-reports/src/kosmickrisp/vulkan/kk_device_fault.h",
  "kosmickrisp-patches/retain-optional-command-queue-fault-reports/src/kosmickrisp/vulkan/kk_device_fault_api.c"
];
const added = file => fs.readFileSync(new URL("./retain-optional-command-queue-fault-reports/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edit("maclauncher/ADAPTATIONS.md", source => source
    .replace("* [shader cache](reference/kosmickrisp-shader-cache.md)\n* [varyings](reference/kosmickrisp-varyings.md)\n* [zero initialization](reference/kosmickrisp-zero-initialization.md)\n",
      () => "* [shader cache](reference/kosmickrisp-shader-cache.md)\n* [varyings](reference/kosmickrisp-varyings.md)\n* [zero initialization](reference/kosmickrisp-zero-initialization.md)\n\n### Retained command-queue fault diagnostics\n\nVulkan contract: diagnostic entrypoints for `VK_KHR_device_fault` and `VK_EXT_device_fault`, subject to the physical-device extension and feature queries.\n\nDiagnostic storage is allocated only for devices enabling `deviceFault`. Queue and sparse-mapping error callbacks copy their error enumeration and UTF-8 message while callback data is live. Reports preserve occurrence order, identify overflow in their bounded queue and support concurrent one-time retrieval. The legacy query retains the first error independently of report consumption. A callback timeout is identified by the bridge's timeout error code.\n\nGPU fault addresses, instruction addresses and vendor binaries are omitted because the public commit feedback supplies none. Vendor codes describe the existing callback error enumeration; a continuation-generated code is not a recovered raw native NSError. Original interlock capture/auxiliary detail follows the existing continuation callback contract. Physical-device capability queries govern availability.\n\nRetention is confined to existing error branches. Successful submission, completion, recording, shader compilation and GPU encoding use their existing paths. Storage is released after queue callbacks have drained.\n\nImplementation: [kk_device_fault.c](../src/kosmickrisp/vulkan/kk_device_fault.c), [kk_device_fault_api.c](../src/kosmickrisp/vulkan/kk_device_fault_api.c), [kk_device_fault.h](../src/kosmickrisp/vulkan/kk_device_fault.h), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [kk_sparse.c](../src/kosmickrisp/vulkan/kk_sparse.c).\n"));
  edit("src/kosmickrisp/vulkan/kk_device.c", source => source
    .replace(" */\n\n#include \"kk_device.h\"\n#include \"kk_async_compute.h\"\n#include \"kosmickrisp-rt-commands.h\"\n#include \"kk_perf.h\"\n",
      () => " */\n\n#include \"kk_device.h\"\n#include \"kk_device_fault.h\"\n#include \"kk_async_compute.h\"\n#include \"kosmickrisp-rt-commands.h\"\n#include \"kk_perf.h\"\n")
    .replace("   kk_descriptor_heap_init(dev);\n   result = kk_vrs_device_init(dev);\n   if (result != VK_SUCCESS) goto fail_init;\n   dev->vk.shader_ops = &kk_device_shader_ops;\n   dev->mtl_handle = pdev->mtl_dev_handle;\n   dev->poly_layer_argument = dev->vk.enabled_features.shaderOutputLayer ||\n",
      () => "   kk_descriptor_heap_init(dev);\n   result = kk_vrs_device_init(dev);\n   if (result != VK_SUCCESS) goto fail_init;\n   result = kk_device_fault_init(dev);\n   if (result != VK_SUCCESS) goto fail_init;\n   dev->vk.shader_ops = &kk_device_shader_ops;\n   dev->mtl_handle = pdev->mtl_dev_handle;\n   dev->poly_layer_argument = dev->vk.enabled_features.shaderOutputLayer ||\n")
    .replace("fail_compiler:\n   kk_release_compiler(dev);\nfail_init:\n   kk_depth_stencil_cache_finish(dev);\n   kk_vrs_device_finish(dev);\n   kk_descriptor_heap_finish(dev);\n",
      () => "fail_compiler:\n   kk_release_compiler(dev);\nfail_init:\n   kk_device_fault_finish(dev);\n   kk_depth_stencil_cache_finish(dev);\n   kk_vrs_device_finish(dev);\n   kk_descriptor_heap_finish(dev);\n")
    .replace("   kk_interlock_submission_finish(dev);\n\n   kk_perf_destroy(dev->perf);\n   vk_device_finish(&dev->vk);\n\n   vk_free(&dev->vk.alloc, dev);\n",
      () => "   kk_interlock_submission_finish(dev);\n\n   kk_perf_destroy(dev->perf);\n   kk_device_fault_finish(dev);\n   vk_device_finish(&dev->vk);\n\n   vk_free(&dev->vk.alloc, dev);\n"));
  edit("src/kosmickrisp/vulkan/kk_device.h", source => source
    .replace("struct kk_frame_capture;\nstruct vk_pipeline_cache;\nstruct kk_perf;\n\nstruct kk_residency_set {\n   simple_mtx_t mutex;\n",
      () => "struct kk_frame_capture;\nstruct vk_pipeline_cache;\nstruct kk_perf;\nstruct kk_device_fault;\n\nstruct kk_residency_set {\n   simple_mtx_t mutex;\n")
    .replace("   struct kk_perf *perf;\n   /* Monotonic: published before the first emulating sampler handle. */\n   uint32_t sampler_emulation;\n   simple_mtx_t depth_stencil_cache_mutex;\n   struct hash_table *depth_stencil_cache;\n   struct kk_sparse_device_state *sparse;\n",
      () => "   struct kk_perf *perf;\n   /* Monotonic: published before the first emulating sampler handle. */\n   uint32_t sampler_emulation;\n   struct kk_device_fault *fault;\n   simple_mtx_t depth_stencil_cache_mutex;\n   struct hash_table *depth_stencil_cache;\n   struct kk_sparse_device_state *sparse;\n"));
  edits.set("src/kosmickrisp/vulkan/kk_device_fault.c", added("src/kosmickrisp/vulkan/kk_device_fault.c"));
  edits.set("src/kosmickrisp/vulkan/kk_device_fault.h", added("src/kosmickrisp/vulkan/kk_device_fault.h"));
  edits.set("src/kosmickrisp/vulkan/kk_device_fault_api.c", added("src/kosmickrisp/vulkan/kk_device_fault_api.c"));
  edit("src/kosmickrisp/vulkan/kk_queue.c", source => source
    .replace("#include \"kk_cmd_buffer.h\"\n#include \"kk_scene.h\"\n#include \"kk_device.h\"\n#include \"kk_entrypoints.h\"\n#include \"kk_physical_device.h\"\n#include \"kk_sync.h\"\n",
      () => "#include \"kk_cmd_buffer.h\"\n#include \"kk_scene.h\"\n#include \"kk_device.h\"\n#include \"kk_device_fault.h\"\n#include \"kk_entrypoints.h\"\n#include \"kk_physical_device.h\"\n#include \"kk_sync.h\"\n")
    .replace("      vk_device_set_lost(\n         &dev->vk, \"Command queue error: %s, with message \\\"%s\\\"\",\n         mtl_command_queue_error_to_string(data->error), data->error_message);\n      kk_interlock_submission_lost(dev);\n   }\n}\n",
      () => "      vk_device_set_lost(\n         &dev->vk, \"Command queue error: %s, with message \\\"%s\\\"\",\n         mtl_command_queue_error_to_string(data->error), data->error_message);\n      kk_device_fault_record(dev, data);\n      kk_interlock_submission_lost(dev);\n   }\n}\n"));
  edit("src/kosmickrisp/vulkan/kk_sparse.c", source => source
    .replace("#include \"kk_buffer.h\"\n#include \"kk_buffer_view.h\"\n#include \"kk_device.h\"\n#include \"kk_device_memory.h\"\n#include \"kk_descriptor_types.h\"\n#include \"kk_format.h\"\n",
      () => "#include \"kk_buffer.h\"\n#include \"kk_buffer_view.h\"\n#include \"kk_device.h\"\n#include \"kk_device_fault.h\"\n#include \"kk_device_memory.h\"\n#include \"kk_descriptor_types.h\"\n#include \"kk_format.h\"\n")
    .replace("   struct kk_sparse_mapping_commit *commit = feedback->user_data;\n   struct kk_queue *queue = commit->queue;\n   struct kk_device *dev = kk_queue_device(queue);\n   if (feedback->error != MTL_COMMAND_QUEUE_ERROR_NONE)\n      vk_device_set_lost(&dev->vk, \"Sparse mapping barrier: %s\", feedback->error_message);\n   mtl_release(commit->command);\n   mtl_release(commit->allocator);\n   kk_sparse_release_copies(dev, &commit->copies);\n",
      () => "   struct kk_sparse_mapping_commit *commit = feedback->user_data;\n   struct kk_queue *queue = commit->queue;\n   struct kk_device *dev = kk_queue_device(queue);\n   if (feedback->error != MTL_COMMAND_QUEUE_ERROR_NONE) {\n      vk_device_set_lost(&dev->vk, \"Sparse mapping barrier: %s\", feedback->error_message);\n      kk_device_fault_record(dev, feedback);\n   }\n   mtl_release(commit->command);\n   mtl_release(commit->allocator);\n   kk_sparse_release_copies(dev, &commit->copies);\n"));
  edit("src/kosmickrisp/vulkan/meson.build", source => source
    .replace("  'kk_device_memory.c',\n  'kk_device.h',\n  'kk_device.c',\n  'kk_perf.c',\n  'kk_atomic64.c',\n  'kk_event.h',\n",
      () => "  'kk_device_memory.c',\n  'kk_device.h',\n  'kk_device.c',\n  'kk_device_fault.c',\n  'kk_device_fault_api.c',\n  'kk_perf.c',\n  'kk_atomic64.c',\n  'kk_event.h',\n"));
}
