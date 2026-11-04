/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_queue.h"
#include "kk_buffer.h"
#include "kk_cmd_buffer.h"
#include "kk_scene.h"
#include "kk_device.h"
#include "kk_device_fault.h"
#include "kk_entrypoints.h"
#include "kk_physical_device.h"
#include "kk_sync.h"
#include "kk_interlock_submit.h"
#include "kk_perf.h"
#include "kk_sparse.h"
#include "kk_temporal.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/vk_to_mtl_map.h"

#include "vk_cmd_queue.h"

#include "kk_queue_deferred.h"

struct kk_commit_data {
   struct kk_queue *queue;
   struct kk_cmd_buffer *cmd;
};

static void
kk_queue_wait_for_commits(struct kk_queue *queue)
{
   struct kk_device *dev = kk_queue_device(queue);
   mtx_lock(&queue->mutex);
   while (queue->commits_in_flight > 0u)
      KK_PERF_VOID(dev, KK_PERF_QUEUE_WAIT,
                   cnd_wait(&queue->cond, &queue->mutex));
   mtx_unlock(&queue->mutex);
}

static void
kk_queue_release_cmd_buffer_locked(struct kk_queue *queue,
                                   struct kk_cmd_buffer *cmd)
{
   kk_cmd_buffer_ops.reset(&cmd->vk, 0u);
   util_dynarray_append(&queue->free_cmd_buffers, &cmd->vk);
}

static void
check_device_lost(struct kk_device *dev, struct mtl_feedback_data *data)
{
   if (data->error != MTL_COMMAND_QUEUE_ERROR_NONE) {
      /* A short GPU time points to a fault and a long one to a timeout. */
      vk_device_set_lost(
         &dev->vk, "Command queue error: %s (%d), with message \"%s\", after %.3f s of GPU time",
         mtl_command_queue_error_to_string(data->error), (int)data->error,
         data->error_message, data->gpu_end - data->gpu_start);
      kk_device_fault_record(dev, data);
      kk_interlock_submission_lost(dev);
   }
}

static void
commit_callback(struct mtl_feedback_data *data)
{
   check_device_lost((struct kk_device *)data->user_data, data);
}

static void
rerecord_commit_callback(struct mtl_feedback_data *data)
{
   struct kk_commit_data *commit = (struct kk_commit_data *)data->user_data;
   struct kk_queue *queue = commit->queue;
   struct kk_device *dev = kk_queue_device(queue);

   check_device_lost(dev, data);

   /* Completion callbacks are called from multiple threads, so we need to
    * ensure the access to queue resources is safe. */
   mtx_lock(&queue->mutex);

   kk_queue_release_cmd_buffer_locked(queue, commit->cmd);

   assert(queue->commits_in_flight > 0u);
   if (--queue->commits_in_flight == 0u) {
      /* Every private replay buffer was already reset before returning
       * to the free list; a zero-flag pool walk adds no cleanup. */
      cnd_broadcast(&queue->cond);
   }

   mtx_unlock(&queue->mutex);

   vk_free(&dev->vk.alloc, commit);
}

static bool
kk_cmd_buffer_has_work(struct kk_cmd_buffer *cmd)
{
   return cmd->metal.cmd_buf;
}

static void
kk_queue_gpu_range(void *data, uint64_t frame, double start, double end,
                   uint32_t count)
{
   kk_perf_gpu_frame(data, frame, start, end, count);
}

static VkResult kk_queue_submit_verbose(struct vk_queue *,
                                         struct vk_queue_submit *);

#include "kk_queue_trace.h"

static VkResult
kk_queue_commit_native(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
                mtl_feedback_handler_callback callback, void *user_data)
{
   assert(kk_cmd_buffer_has_work(cmd));
   if (cmd->interlock_segments)
      return kk_interlock_commit(queue, cmd, callback, user_data);
   /* Feedback may reset/recycle cmd before native commit returns. Move the
    * retained drawables out first; post-commit code must not touch cmd storage. */
   struct util_dynarray owned_drawables = cmd->submit_drawables;
   cmd->submit_drawables = UTIL_DYNARRAY_INIT;
   void *primary_drawable = cmd->drawable;
   cmd->drawable = NULL;
   const uint32_t retained_count = util_dynarray_num_elements(&owned_drawables, void *);
   void **drawables = util_dynarray_begin(&owned_drawables);
   bool primary_retained = false;
   for (uint32_t i = 0; i < retained_count; i++)
      primary_retained |= drawables[i] == primary_drawable;
   const uint32_t count = retained_count +
      (primary_drawable != NULL && !primary_retained);
   for (uint32_t i = 0; i < count; i++) {
      void *drawable = i < retained_count ? drawables[i] : primary_drawable;
      KK_PERF_VOID(kk_queue_device(queue), KK_PERF_DRAWABLE_WAIT,
                   mtl_command_queue_wait_for_drawable(queue->mtl_handle,
                                                        drawable));
   }

   mtl_commit_options *options = mtl_new_commit_options();
   if (!options) {
      cmd->submit_drawables = owned_drawables;
      cmd->drawable = primary_drawable;
      return vk_error(kk_queue_device(queue), VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   /* Forwarded temporal frames run between parts; the last commit carries the feedback. */
   const uint32_t first = unlikely(cmd->temporal_records.size) ?
      kk_temporal_commit_prefix(queue, cmd) : 0;
   mtl_command_buffer **buffers = cmd->native_submit.buffers + first;
   const uint32_t parts = cmd->native_submit.count - first;
   struct kk_perf *perf = kk_queue_device(queue)->perf;
   if (unlikely(perf)) {
      if (queue->native_submit == kk_queue_submit_verbose) {
         kk_queue_commit_tracked(queue, cmd, buffers, parts, options, callback, user_data, perf);
      } else {
         mtl_commit_options_add_tracked_feedback_handler(
            options, callback, user_data, kk_queue_gpu_range, perf,
            kk_perf_frame_id(perf), parts, buffers);
         mtl_command_queue_commit(queue->mtl_handle, buffers, parts, options);
      }
   } else {
      mtl_commit_options_add_feedback_handler(options, callback, user_data);
      mtl_command_queue_commit(queue->mtl_handle, buffers, parts, options);
   }
   mtl_release(options);
   for (uint32_t i = 0; i < count; i++) {
      void *drawable = i < retained_count ? drawables[i] : primary_drawable;
      mtl_command_queue_signal_drawable(queue->mtl_handle, drawable);
   }
   for (uint32_t i = 0; i < retained_count; i++)
      mtl_release(drawables[i]);
   util_dynarray_fini(&owned_drawables);
   if (primary_drawable) {
      mtl_release(primary_drawable);
   }
   return VK_SUCCESS;
}

/* Detach flight storage before native submission: feedback may run before
 * commit returns. The original pool/retirement callback runs after feedback. */
static VkResult
kk_queue_commit(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
                mtl_feedback_handler_callback callback, void *data)
{
   struct kk_scene_submission *scene;
   VkResult result = kk_scene_submission_begin(queue, cmd, callback, data, &scene);
   if (result != VK_SUCCESS) return result;
   bool deferred = cmd->interlock_segments != NULL;
   result = kk_queue_commit_native(queue, cmd,
      scene ? kk_scene_submission_feedback : callback, scene ? scene : data);
   if (result != VK_SUCCESS) kk_scene_submission_failed(scene);
   else if (!deferred) kk_scene_submission_committed(scene, queue);
   kk_scene_submission_release(scene); /* caller handoff */
   return result;
}

#include "kk_sparse_queue.h"

static VkResult
rerecord_and_commit_cmd_buffers(struct kk_queue *queue,
                                struct vk_command_buffer **commands,
                                uint32_t count, uint32_t *consumed, bool *committed)
{
   struct kk_device *dev = kk_queue_device(queue);
   struct kk_cmd_buffer *source = container_of(commands[0], struct kk_cmd_buffer, vk);
   if (source->scene_native_tail) {
      /* A one-time direct prefix has no IR to replay into a private command.
       * Continue its own retained state, then commit all its native parts. */
      *consumed = 1;
      *committed = false;
      if (!source->metal.cmd_buf) {
         VkResult result = kk_scene_submission_reserve(queue, source);
         if (result != VK_SUCCESS) return result;
         result = kk_cmd_buffer_resume_native_recording(source);
         if (result != VK_SUCCESS) {
            kk_scene_recording_failed(source);
            return result;
         }
         source->scene_queue = queue;
         KK_PERF_VOID(dev, KK_PERF_REPLAY,
            kk_mesh_execute_command_queue(&source->vk.cmd_queue,
               kk_cmd_buffer_to_handle(source), &dev->vk.dispatch_table));
         kk_cmd_buffer_end_native_recording(source);
      }
      if (source->vk.record_result != VK_SUCCESS) {
         kk_scene_recording_failed(source);
         return source->vk.record_result;
      }
      kk_device_make_resources_resident(dev);
      VkResult result = kk_sparse_commit_first(queue, source);
      *committed = result == VK_SUCCESS;
      return result;
   }
   struct vk_command_buffer *vk_cmd = NULL;
   *committed = false;
   struct kk_cmd_buffer *rerecord;
   VkResult result = VK_SUCCESS;

   /* Completion callbacks are called from multiple threads, so we need to
    * ensure the access to queue resources is safe. */
   mtx_lock(&queue->mutex);

   if (util_dynarray_num_elements(&queue->free_cmd_buffers,
                                  struct vk_command_buffer *) > 0u) {
      vk_cmd = util_dynarray_pop(&queue->free_cmd_buffers,
                                 struct vk_command_buffer *);
   } else {
      result = kk_cmd_buffer_ops.create(
         &queue->cmd_pool->vk, VK_COMMAND_BUFFER_LEVEL_PRIMARY, &vk_cmd);
      if (result != VK_SUCCESS)
         goto unlock;
   }

   rerecord = container_of(vk_cmd, struct kk_cmd_buffer, vk);
   VkCommandBuffer rerecord_handle = kk_cmd_buffer_to_handle(rerecord);
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   result = kk_BeginCommandBuffer(rerecord_handle, &begin_info);
   if (result != VK_SUCCESS)
      goto release;

   rerecord->scene_queue = queue;
   /* Eligibility belongs to the complete immutable recorded command list.
    * Combining command-buffer summaries avoids rebinding every pipeline on
    * each submission, including unrelated trailing command buffers. */
   rerecord->transient_safe = true;
   bool has_indirect_mesh = false;
   for (uint32_t i = 0; i < count; i++) {
      struct kk_cmd_buffer *source =
         container_of(commands[i], struct kk_cmd_buffer, vk);
      rerecord->transient_safe &= source->transient_safe;
      rerecord->uses_occlusion_queries |= source->uses_occlusion_queries;
      has_indirect_mesh |= source->uses_indirect_mesh;
   }
   /* Preserve the source-list classification through End's empty replay IR. */
   rerecord->transient_preflight_done = true;
   for (*consumed = 0; *consumed < count; (*consumed)++) {
      if (*consumed)
         kk_whole_pass_command_boundary(rerecord);
      uint32_t chain = kk_mesh_rendering_chain_length(rerecord,
         commands + *consumed, count - *consumed, has_indirect_mesh);
      for (uint32_t j = 0; j < chain; ++j) {
         struct kk_cmd_buffer *part = container_of(commands[*consumed + j], struct kk_cmd_buffer, vk);
         rerecord->uses_indirect_mesh |= part->uses_indirect_mesh;
         if (part->drawable) {
            bool retained = false;
            util_dynarray_foreach(&rerecord->submit_drawables, void *, drawable)
               retained |= *drawable == part->drawable;
            if (!retained) {
               void **drawable = util_dynarray_grow(
                  &rerecord->submit_drawables, void *, 1);
               if (!drawable) {
                  result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
                  goto release;
               }
               *drawable = mtl_retain(part->drawable);
            }
         }
      }
      KK_PERF_VOID(dev, KK_PERF_REPLAY,
         kk_mesh_execute_command_buffers(commands + *consumed, chain, rerecord_handle,
                                         &dev->vk.dispatch_table));
      *consumed += chain - 1;
      if (!rerecord->whole_pass.suspended) {
         (*consumed)++;
         break;
      }
   }

   result = kk_EndCommandBuffer(rerecord_handle);
   if (result != VK_SUCCESS)
      goto release;

   if (!kk_cmd_buffer_has_work(rerecord))
      goto release;

   struct kk_commit_data *commit = vk_alloc(&dev->vk.alloc, sizeof(*commit), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (commit == NULL) {
      result = vk_error(queue, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto release;
   }
   commit->queue = queue;
   commit->cmd = rerecord;

   /* Need to ensure the new buffers allocated at record are resident. */
   kk_device_make_resources_resident(dev);

   queue->commits_in_flight++;
   const bool sparse_tracked = rerecord->uses_sparse_buffers &&
      kk_sparse_submission_begin(dev);
   mtl_feedback_handler_callback callback = sparse_tracked ?
      kk_sparse_rerecord_commit_callback : rerecord_commit_callback;
   result = kk_queue_commit(queue, rerecord, callback, commit);
   if (result != VK_SUCCESS) {
      if (sparse_tracked)
         kk_sparse_submission_end(dev);
      assert(queue->commits_in_flight > 0u);
      if (--queue->commits_in_flight == 0u)
         cnd_broadcast(&queue->cond);
      vk_free(&dev->vk.alloc, commit);
      goto release;
   }
   *committed = true;

   mtx_unlock(&queue->mutex);
   return VK_SUCCESS;

release:
   kk_queue_release_cmd_buffer_locked(queue, rerecord);
unlock:
   mtx_unlock(&queue->mutex);
   return result;
}

static VkResult
kk_queue_submit_internal(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct kk_queue *queue = container_of(vk_queue, struct kk_queue, vk);
   struct kk_device *dev = kk_queue_device(queue);

   if (vk_queue_is_lost(&queue->vk))
      return VK_ERROR_DEVICE_LOST;

   /* An internally synchronized submit may already have selected the
    * immediate path when another thread registered a continuation. Queue it
    * behind that retained owner rather than resuming the owner's callback. */
   if (queue->deferred.resume && queue->deferred.submit != submit)
      return VK_NOT_READY;

   const bool resuming = queue->deferred.resume != NULL;
   uint32_t first_command = 0;
   if (resuming) {
      VkResult result = kk_queue_resume_deferred(queue, false);
      if (result != VK_SUCCESS)
         return result;
      if (queue->deferred.resume)
         return VK_NOT_READY;
      first_command = queue->deferred.next_command;
   }

   if (!resuming) {
   for (struct vk_sync_wait *wait = submit->waits,
                            *end = submit->waits + submit->wait_count;
        wait != end; ++wait) {
      struct kk_sync_timeline *sync =
         container_of(wait->sync, struct kk_sync_timeline, base);
      mtl_wait_for_event(queue->mtl_handle, sync->mtl_handle, wait->wait_value);
   }

   /* Ensure any changes to residency are propagated before we submit any
    * work. All resources should have been allocated before submission.
    * Otherwise, users are playing with fire. */
   kk_device_make_resources_resident(dev);

   if (vk_queue_submit_has_bind(submit)) {
      VkResult result = kk_queue_bind_sparse(queue, submit);
      if (result != VK_SUCCESS) return result;
   }
   }
   for (uint32_t i = first_command; i < submit->command_buffer_count; ++i) {
      struct kk_cmd_buffer *cmd_buffer =
         container_of(submit->command_buffers[i], struct kk_cmd_buffer, vk);

      /* Metal's command buffers are one time use, re-record multiple
       * submissions. */
      if (cmd_buffer->submitted || cmd_buffer->enqueue_commands ||
          cmd_buffer->deferred_render_chain ||
          (kk_sparse_residency_used(dev) & ~cmd_buffer->sparse_guarded)) {
         uint32_t consumed = 0;
         bool committed = false;
         VkResult result = rerecord_and_commit_cmd_buffers(queue,
            &submit->command_buffers[i], submit->command_buffer_count - i,
            &consumed, &committed);
         if (result != VK_SUCCESS)
            return result == VK_ERROR_DEVICE_LOST
               ? kk_interlock_submission_lost(dev) : result;
         for (uint32_t j = 0; committed && j < consumed; j++) {
            struct kk_cmd_buffer *source = container_of(
               submit->command_buffers[i + j], struct kk_cmd_buffer, vk);
            source->submitted = true;
            if (source->drawable) {
               mtl_release(source->drawable);
               source->drawable = NULL;
            }
         }
         i += consumed - 1;
         if (queue->deferred.resume) {
            queue->deferred.next_command = i + 1;
            return VK_NOT_READY;
         }
         continue;
      } else if (kk_cmd_buffer_has_work(cmd_buffer)) {
         VkResult result = kk_sparse_commit_first(queue, cmd_buffer);
         if (result != VK_SUCCESS)
            return result == VK_ERROR_DEVICE_LOST ?
               kk_interlock_submission_lost(dev) : result;
      }

      cmd_buffer->submitted = true;
      if (queue->deferred.resume) {
         queue->deferred.next_command = i + 1;
         return VK_NOT_READY;
      }

   }

   for (uint32_t i = 0u; i < submit->signal_count; ++i) {
      struct vk_sync_signal *signal = &submit->signals[i];
      struct kk_sync_timeline *sync =
         container_of(signal->sync, struct kk_sync_timeline, base);
      kk_sync_publish_begin(sync);
      mtl_signal_event(queue->mtl_handle, sync->mtl_handle,
                       signal->signal_value);
      kk_sync_mark_pending(sync, signal->signal_value);
      kk_sync_publish_end(sync);
   }

   return VK_SUCCESS;
}

static VkResult
kk_queue_submit(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   return kk_queue_submit_internal(vk_queue, submit);
}

static VkResult
kk_queue_submit_metrics(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct kk_queue *queue = container_of(vk_queue, struct kk_queue, vk);
   struct kk_device *dev = kk_queue_device(queue);
   return KK_PERF_CALL(dev, KK_PERF_SUBMIT,
                       kk_queue_submit_internal(vk_queue, submit));
}

static VkResult
kk_queue_submit_verbose(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct kk_queue *queue = container_of(vk_queue, struct kk_queue, vk);
   struct kk_device *dev = kk_queue_device(queue);
   uint64_t id = kk_perf_submit_trace(dev->perf, "begin", 0, queue, submit, 0);
   VkResult result = kk_queue_submit_metrics(vk_queue, submit);
   kk_perf_submit_trace(dev->perf, "return", id, queue, submit, result);
   return result;
}

VkResult
kk_queue_init(struct kk_device *dev, struct kk_queue *queue,
              const VkDeviceQueueCreateInfo *pCreateInfo,
              uint32_t index_in_family)
{
   const VkDeviceQueueGlobalPriorityCreateInfo *priority_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO);
   const VkQueueGlobalPriority global_priority =
      priority_info ? priority_info->globalPriority
                    : VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;

   /* From the Vulkan 1.3.295 spec:
    *
    *    "If the globalPriorityQuery feature is enabled and the requested
    *    global priority is not reported via
    *    VkQueueFamilyGlobalPriorityPropertiesKHR, the driver implementation
    *    must fail the queue creation. In this scenario,
    *    VK_ERROR_INITIALIZATION_FAILED is returned."
    */
   if (dev->vk.enabled_features.globalPriorityQuery &&
       global_priority != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (global_priority > VK_QUEUE_GLOBAL_PRIORITY_MEDIUM)
      return VK_ERROR_NOT_PERMITTED;

   VkResult result;

   result = vk_queue_init(&queue->vk, &dev->vk, pCreateInfo, index_in_family);
   if (result != VK_SUCCESS)
      return result;

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = pCreateInfo->queueFamilyIndex,
   };
   VkCommandPool pool_handle;
   result = kk_CreateCommandPool(kk_device_to_handle(dev), &pool_info, NULL,
                                 &pool_handle);
   if (result != VK_SUCCESS) {
      vk_queue_finish(&queue->vk);
      return result;
   }
   queue->cmd_pool = kk_cmd_pool_from_handle(pool_handle);

   queue->free_cmd_buffers = UTIL_DYNARRAY_INIT;
   queue->commits_in_flight = 0u;
   queue->interlock_submission = NULL;
   mtx_init(&queue->mutex, mtx_plain);
   cnd_init(&queue->cond);

   queue->mtl_handle = mtl_new_command_queue(dev->mtl_handle);
   if (!queue->mtl_handle) {
      cnd_destroy(&queue->cond);
      mtx_destroy(&queue->mutex);
      util_dynarray_fini(&queue->free_cmd_buffers);
      kk_DestroyCommandPool(kk_device_to_handle(dev), pool_handle, NULL);
      vk_queue_finish(&queue->vk);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   mtl_command_queue_add_residency_set(queue->mtl_handle,
                                       dev->residency_set.handle);

   queue->native_submit = !dev->perf ? kk_queue_submit :
      kk_perf_submit_details(dev->perf) ? kk_queue_submit_verbose :
                                        kk_queue_submit_metrics;
   queue->vk.driver_submit = queue->native_submit;

   return VK_SUCCESS;
}

void
kk_queue_finish(struct kk_device *dev, struct kk_queue *queue)
{
   /* Join the Mesa worker before freeing its command pools or the
    * caller's deferred payload. Lost waits terminate through vk_sync. */
   vk_queue_finish(&queue->vk);
   if (queue->deferred.resume)
      (void)kk_queue_resume_deferred(queue, true);

   /* The common worker and its one cancel callback are finished. Drain all
    * interlock C packets/native callbacks while pools and mutex remain live. */
   kk_interlock_queue_finish(queue);
   /* There may be in-flight command buffers, wait for them to finish. */
   kk_queue_wait_for_commits(queue);

   mtl_command_queue_remove_residency_set(queue->mtl_handle,
                                          dev->residency_set.handle);
   mtl_release(queue->mtl_handle);
   kk_temporal_queue_finish(queue);

   kk_DestroyCommandPool(kk_device_to_handle(dev),
                         kk_cmd_pool_to_handle(queue->cmd_pool), NULL);
   util_dynarray_fini(&queue->free_cmd_buffers);
   cnd_destroy(&queue->cond);
   mtx_destroy(&queue->mutex);
}
