// SPDX-License-Identifier: MIT
// Record native scene operations with retained submission flights
import fs from 'node:fs';
export const id = "record-native-scene-operations-with-retained-submission-flights";
export const sourceFiles = [
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/maclauncher/SCENE_OPERATIONS.md",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/kk_scene.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/kk_scene.m",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/kk_scene_lifetime.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/generation-provider.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/image-color.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/recorded-scene.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/scene-interop.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/temporal-frame.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/temporal-scene-aovs.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/temporal-scene-matrices.h",
  "kosmickrisp-patches/record-native-scene-operations-with-retained-submission-flights/src/kosmickrisp/vulkan/scene/vulkan-scene.h"
];
const added = file => fs.readFileSync(new URL("./record-native-scene-operations-with-retained-submission-flights/" + file, import.meta.url), 'utf8');
export function apply({edit, edits}) {
  edit("maclauncher/ADAPTATIONS.md", source => source
    .replace("\nImplementation: [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c).\n\n## Detailed specifications\n\n* [ATOMIC INT64](reference/KOSMICKRISP_ATOMIC_INT64.md)\n",
      () => "\nImplementation: [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c).\n\n### Recorded native scene operations\n\nOptional scene operation API3 records on the producer's actual device and Metal4 queue. Each execution retains its resolved image subresources and stable result storage through submission, completion, failure or abandonment. Shape queries and resource preparation use the selected common provider; recording success alone does not fulfill an output. API1/API2 layouts and the EncodingV2 payload remain unchanged.\n\nImplementation: [kk_scene.m](../src/kosmickrisp/vulkan/kk_scene.m), [scene operation contract](SCENE_OPERATIONS.md).\n\n## Detailed specifications\n\n* [ATOMIC INT64](reference/KOSMICKRISP_ATOMIC_INT64.md)\n"));
  edits.set("maclauncher/SCENE_OPERATIONS.md", added("maclauncher/SCENE_OPERATIONS.md"));
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.c", source => source
    .replace("\n#include \"kk_cmd_buffer.h\"\n#include \"kk_perf.h\"\n#include \"kk_depth_stencil_state.h\"\n#include \"kk_interlock_submit.h\"\n#include \"kk_interlock_heap.h\"\n",
      () => "\n#include \"kk_cmd_buffer.h\"\n#include \"kk_perf.h\"\n#include \"kk_scene.h\"\n#include \"kk_depth_stencil_state.h\"\n#include \"kk_interlock_submit.h\"\n#include \"kk_interlock_heap.h\"\n")
    .replace("static void\nkk_release_command_allocators(struct kk_cmd_buffer *cmd)\n{\n   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);\n\n   /* Native buffers must return their recorded storage before reset\n",
      () => "static void\nkk_release_command_allocators(struct kk_cmd_buffer *cmd)\n{\n   kk_scene_reset(cmd);\n   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);\n\n   /* Native buffers must return their recorded storage before reset\n")
    .replace("{\n   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);\n\n   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY &&\n       cmd->vk.record_result == VK_SUCCESS) {\n      struct vk_command_buffer *source = &cmd->vk;\n",
      () => "{\n   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);\n\n   /* Native scene callbacks need the actual submission queue. The\n    * existing replay pool preserves order and owns each execution's storage. */\n   if (kk_scene_needs_submit(cmd))\n      cmd->enqueue_commands = true;\n   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY &&\n       cmd->vk.record_result == VK_SUCCESS) {\n      struct vk_command_buffer *source = &cmd->vk;\n"));
  edit("src/kosmickrisp/vulkan/kk_cmd_buffer.h", source => source
    .replace("\nstruct kk_cmd_buffer {\n   struct vk_command_buffer vk;\n   struct kk_depth_stencil_record *depth_stencil_records;\n   struct kk_interlock_segment *interlock_segments;\n   struct kk_interlock_segment *interlock_segments_tail;\n",
      () => "\nstruct kk_cmd_buffer {\n   struct vk_command_buffer vk;\n   struct kk_queue *scene_queue;\n   struct kk_scene_record *scene_records;\n   struct kk_scene_flight *scene_flights;\n   struct kk_depth_stencil_record *depth_stencil_records;\n   struct kk_interlock_segment *interlock_segments;\n   struct kk_interlock_segment *interlock_segments_tail;\n"));
  edit("src/kosmickrisp/vulkan/kk_interlock_submit.c", source => source
    .replace("/* SPDX-License-Identifier: MIT */\n#include \"kk_interlock_submit.h\"\n#include \"kk_interlock_heap.h\"\n#include \"kk_device.h\"\n#include \"kk_queue.h\"\n",
      () => "/* SPDX-License-Identifier: MIT */\n#include \"kk_interlock_submit.h\"\n#include \"kk_scene.h\"\n#include \"kk_interlock_heap.h\"\n#include \"kk_device.h\"\n#include \"kk_queue.h\"\n")
    .replace("   atomic_int error;\n   _Atomic(mtl_feedback_handler_callback) completed;\n   void *callback_data;\n   struct kk_device *device;\n   struct kk_queue *queue;\n   struct kk_interlock_queue *owner;\n",
      () => "   atomic_int error;\n   _Atomic(mtl_feedback_handler_callback) completed;\n   void *callback_data;\n   struct kk_scene_submission *scene_submission;\n   struct kk_device *device;\n   struct kk_queue *queue;\n   struct kk_interlock_queue *owner;\n")
    .replace("{\n   if (atomic_fetch_sub_explicit(&p->references, 1, memory_order_acq_rel) != 1)\n      return;\n   struct kk_device *dev = p->device;\n   struct kk_interlock_registry *registry = dev->interlock_registry;\n   struct kk_interlock_queue *owner = p->owner;\n",
      () => "{\n   if (atomic_fetch_sub_explicit(&p->references, 1, memory_order_acq_rel) != 1)\n      return;\n   kk_scene_submission_release(p->scene_submission);\n   struct kk_device *dev = p->device;\n   struct kk_interlock_registry *registry = dev->interlock_registry;\n   struct kk_interlock_queue *owner = p->owner;\n")
    .replace("         mtl_commit_options_add_feedback_handler(options,\n            kk_interlock_packet_tail_completed, p);\n         mtl_command_queue_commit(queue->mtl_handle, p->tail, p->tail_count, options);\n         kk_interlock_signal_drawables(p);\n         mtl_release(options);\n      }\n",
      () => "         mtl_commit_options_add_feedback_handler(options,\n            kk_interlock_packet_tail_completed, p);\n         mtl_command_queue_commit(queue->mtl_handle, p->tail, p->tail_count, options);\n         kk_scene_submission_committed(p->scene_submission, queue);\n         kk_interlock_signal_drawables(p);\n         mtl_release(options);\n      }\n")
    .replace("   atomic_init(&p->drawables_waited, false);\n   atomic_init(&p->drawables_signalled, false);\n   p->callback_data = data;\n   p->device = dev;\n   p->queue = queue;\n   p->count = count;\n",
      () => "   atomic_init(&p->drawables_waited, false);\n   atomic_init(&p->drawables_signalled, false);\n   p->callback_data = data;\n   p->scene_submission = kk_scene_submission_acquire(callback, data);\n   p->device = dev;\n   p->queue = queue;\n   p->count = count;\n"));
  edit("src/kosmickrisp/vulkan/kk_queue.c", source => source
    .replace("#include \"kk_queue.h\"\n#include \"kk_buffer.h\"\n#include \"kk_cmd_buffer.h\"\n#include \"kk_device.h\"\n#include \"kk_entrypoints.h\"\n#include \"kk_physical_device.h\"\n",
      () => "#include \"kk_queue.h\"\n#include \"kk_buffer.h\"\n#include \"kk_cmd_buffer.h\"\n#include \"kk_scene.h\"\n#include \"kk_device.h\"\n#include \"kk_entrypoints.h\"\n#include \"kk_physical_device.h\"\n")
    .replace("}\n\nstatic VkResult\nkk_queue_commit(struct kk_queue *queue, struct kk_cmd_buffer *cmd,\n                mtl_feedback_handler_callback callback, void *user_data)\n{\n   assert(kk_cmd_buffer_has_work(cmd));\n",
      () => "}\n\nstatic VkResult\nkk_queue_commit_native(struct kk_queue *queue, struct kk_cmd_buffer *cmd,\n                mtl_feedback_handler_callback callback, void *user_data)\n{\n   assert(kk_cmd_buffer_has_work(cmd));\n")
    .replace("   return VK_SUCCESS;\n}\n\n#include \"kk_sparse_queue.h\"\n\nstatic VkResult\n",
      () => "   return VK_SUCCESS;\n}\n\n/* Detach flight storage before native submission: feedback may run before\n * commit returns. The original pool/retirement callback runs after feedback. */\nstatic VkResult\nkk_queue_commit(struct kk_queue *queue, struct kk_cmd_buffer *cmd,\n                mtl_feedback_handler_callback callback, void *data)\n{\n   struct kk_scene_submission *scene;\n   VkResult result = kk_scene_submission_begin(queue, cmd, callback, data, &scene);\n   if (result != VK_SUCCESS) return result;\n   bool deferred = cmd->interlock_segments != NULL;\n   result = kk_queue_commit_native(queue, cmd,\n      scene ? kk_scene_submission_feedback : callback, scene ? scene : data);\n   if (result != VK_SUCCESS) kk_scene_submission_failed(scene);\n   else if (!deferred) kk_scene_submission_committed(scene, queue);\n   kk_scene_submission_release(scene); /* caller handoff */\n   return result;\n}\n\n#include \"kk_sparse_queue.h\"\n\nstatic VkResult\n")
    .replace("   if (result != VK_SUCCESS)\n      goto release;\n\n   /* Eligibility belongs to the complete immutable recorded command list.\n    * Combining command-buffer summaries avoids rebinding every pipeline on\n    * each submission, including unrelated trailing command buffers. */\n",
      () => "   if (result != VK_SUCCESS)\n      goto release;\n\n   rerecord->scene_queue = queue;\n   /* Eligibility belongs to the complete immutable recorded command list.\n    * Combining command-buffer summaries avoids rebinding every pipeline on\n    * each submission, including unrelated trailing command buffers. */\n"));
  edits.set("src/kosmickrisp/vulkan/kk_scene.h", added("src/kosmickrisp/vulkan/kk_scene.h"));
  edits.set("src/kosmickrisp/vulkan/kk_scene.m", added("src/kosmickrisp/vulkan/kk_scene.m"));
  edits.set("src/kosmickrisp/vulkan/kk_scene_lifetime.h", added("src/kosmickrisp/vulkan/kk_scene_lifetime.h"));
  edit("src/kosmickrisp/vulkan/meson.build", source => source
    .replace("  'kk_buffer.c',\n  'kk_cmd_buffer.h',\n  'kk_cmd_buffer.c',\n  'kk_cmd_clear.c',\n  'kk_cmd_copy.c',\n  'kk_cmd_dispatch.c',\n",
      () => "  'kk_buffer.c',\n  'kk_cmd_buffer.h',\n  'kk_cmd_buffer.c',\n  'kk_scene.m',\n  'kk_cmd_clear.c',\n  'kk_cmd_copy.c',\n  'kk_cmd_dispatch.c',\n"));
  edits.set("src/kosmickrisp/vulkan/scene/generation-provider.h", added("src/kosmickrisp/vulkan/scene/generation-provider.h"));
  edits.set("src/kosmickrisp/vulkan/scene/image-color.h", added("src/kosmickrisp/vulkan/scene/image-color.h"));
  edits.set("src/kosmickrisp/vulkan/scene/recorded-scene.h", added("src/kosmickrisp/vulkan/scene/recorded-scene.h"));
  edits.set("src/kosmickrisp/vulkan/scene/scene-interop.h", added("src/kosmickrisp/vulkan/scene/scene-interop.h"));
  edits.set("src/kosmickrisp/vulkan/scene/temporal-frame.h", added("src/kosmickrisp/vulkan/scene/temporal-frame.h"));
  edits.set("src/kosmickrisp/vulkan/scene/temporal-scene-aovs.h", added("src/kosmickrisp/vulkan/scene/temporal-scene-aovs.h"));
  edits.set("src/kosmickrisp/vulkan/scene/temporal-scene-matrices.h", added("src/kosmickrisp/vulkan/scene/temporal-scene-matrices.h"));
  edits.set("src/kosmickrisp/vulkan/scene/vulkan-scene.h", added("src/kosmickrisp/vulkan/scene/vulkan-scene.h"));
  edit("src/vulkan/util/vk_cmd_queue_gen.py", source => source
    .replace("#endif // ${c.guard}\n% endif\n% endfor\n   VK_CMD_TYPE_COUNT,\n};\n\n",
      () => "#endif // ${c.guard}\n% endif\n% endfor\n   VK_CMD_DRIVER,\n   VK_CMD_TYPE_COUNT,\n};\n\n")
    .replace("   struct list_head cmd_link;\n   enum vk_cmd_type type;\n   union {\n% for c in commands:\n% if len(c.params) <= 1:\n<% continue %>\n",
      () => "   struct list_head cmd_link;\n   enum vk_cmd_type type;\n   union {\n      /* Internal native operation in the same ordered command IR. Its driver\n       * owns recording data; ordinary generated Vulkan commands are unchanged. */\n      struct { void *data; void (*execute)(VkCommandBuffer, void *); } driver;\n% for c in commands:\n% if len(c.params) <= 1:\n<% continue %>\n")
    .replace("#endif // ${c.guard}\n% endif\n% endfor\n};\n\nsize_t vk_cmd_queue_type_sizes[] = {\n",
      () => "#endif // ${c.guard}\n% endif\n% endfor\n   \"VK_CMD_DRIVER\",\n};\n\nsize_t vk_cmd_queue_type_sizes[] = {\n")
    .replace("#endif // ${c.guard}\n% endif\n% endfor\n};\n\n/* From the application's perspective, the vk_cmd_queue_entry can outlive the\n",
      () => "#endif // ${c.guard}\n% endif\n% endfor\n   sizeof(struct vk_cmd_queue_entry),\n};\n\n/* From the application's perspective, the vk_cmd_queue_entry can outlive the\n")
    .replace("{\n   list_for_each_entry(struct vk_cmd_queue_entry, cmd, &queue->cmds, cmd_link) {\n      switch (cmd->type) {\n% for c in commands:\n% if c.guard is not None:\n#ifdef ${c.guard}\n",
      () => "{\n   list_for_each_entry(struct vk_cmd_queue_entry, cmd, &queue->cmds, cmd_link) {\n      switch (cmd->type) {\n      case VK_CMD_DRIVER:\n         cmd->u.driver.execute(commandBuffer, cmd->u.driver.data);\n         break;\n% for c in commands:\n% if c.guard is not None:\n#ifdef ${c.guard}\n"));
}
