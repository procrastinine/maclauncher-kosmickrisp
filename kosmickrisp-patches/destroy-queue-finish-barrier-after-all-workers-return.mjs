// SPDX-License-Identifier: MIT
// Destroy queue finish barrier after all workers return
export const id = "destroy-queue-finish-barrier-after-all-workers-return";
export function apply({edit, edits}) {
  edit("src/util/u_queue.c", source => source
    .replace("util_queue_finish_execute(void *data, void *gdata, int num_thread)\n{\n   util_barrier *barrier = data;\n   if (util_barrier_wait(barrier))\n      util_barrier_destroy(barrier);\n}\n\nvoid\n",
      () => "util_queue_finish_execute(void *data, void *gdata, int num_thread)\n{\n   util_barrier *barrier = data;\n   util_barrier_wait(barrier);\n}\n\nvoid\n")
    .replace("      util_queue_fence_wait(&fences[i]);\n      util_queue_fence_destroy(&fences[i]);\n   }\n   simple_mtx_unlock(&queue->finish_lock);\n\n   free(fences);\n",
      () => "      util_queue_fence_wait(&fences[i]);\n      util_queue_fence_destroy(&fences[i]);\n   }\n   /* Every execute callback has returned before its fence is signalled.\n    * In particular, no participant can still be waking/relocking inside the\n    * barrier when its condition variable and mutex are destroyed. */\n   util_barrier_destroy(&barrier);\n   simple_mtx_unlock(&queue->finish_lock);\n\n   free(fences);\n"));
}
