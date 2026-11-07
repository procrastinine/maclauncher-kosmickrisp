/* SPDX-License-Identifier: MIT */
#ifndef KK_QUEUE_TRACE_H
#define KK_QUEUE_TRACE_H

/* Only the existing perf branch calls this helper. A private snapshot avoids
 * reading command storage that an inline feedback callback can retire before
 * the native commit call returns. Both callback and caller own a reference. */
struct kk_tracked_commit {
   struct kk_perf *perf;
   const void *queue, *submit, *command, *native;
   uint64_t id;
   uint32_t count;
   int refs;
   mtl_feedback_handler_callback callback;
   void *data;
};

static void
kk_tracked_commit_release(struct kk_tracked_commit *trace)
{
   if (p_atomic_dec_zero(&trace->refs))
      free(trace);
}

static void
kk_tracked_commit_feedback(struct mtl_feedback_data *feedback)
{
   struct kk_tracked_commit *trace = feedback->user_data;
   kk_perf_commit_trace(trace->perf, "complete", trace->id, trace->queue,
      trace->submit, trace->command, trace->native, trace->count, feedback->error);
   struct mtl_feedback_data original = *feedback;
   original.user_data = trace->data;
   trace->callback(&original);
   kk_tracked_commit_release(trace);
}

static void
kk_queue_commit_tracked(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
   mtl_commit_options *options, mtl_feedback_handler_callback callback,
   void *user_data, struct kk_perf *perf)
{
   const void *submit = queue->vk.driver_submit_active;
   const void *native = cmd->metal.cmd_buf;
   const uint32_t count = cmd->native_submit.count;
   const uint64_t id = kk_perf_commit_trace(perf, "begin", 0, queue, submit,
      cmd, native, count, 0);
   struct kk_tracked_commit *trace = malloc(sizeof(*trace));
   if (trace) {
      *trace = (struct kk_tracked_commit) {
         .perf = perf, .queue = queue, .submit = submit, .command = cmd,
         .native = native, .count = count, .id = id, .refs = 2,
         .callback = callback, .data = user_data,
      };
      mtl_commit_options_add_tracked_feedback_handler(options,
         kk_tracked_commit_feedback, trace, kk_queue_gpu_range, perf,
         kk_perf_frame_id(perf), count);
   } else {
      /* Diagnostic allocation failure must not drop the real callback/work. */
      kk_perf_commit_trace(perf, "snapshot_oom", id, queue, submit,
         cmd, native, count, VK_ERROR_OUT_OF_HOST_MEMORY);
      mtl_commit_options_add_tracked_feedback_handler(options,
         callback, user_data, kk_queue_gpu_range, perf,
         kk_perf_frame_id(perf), count);
   }
   mtl_command_queue_commit(queue->mtl_handle, cmd->native_submit.buffers,
                            count, options);
   kk_perf_commit_trace(perf, "return", id, queue, submit, cmd, native, count, 0);
   if (trace)
      kk_tracked_commit_release(trace);
}
#endif
