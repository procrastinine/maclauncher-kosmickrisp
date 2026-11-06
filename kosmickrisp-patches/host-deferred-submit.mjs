// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'host-deferred-submit';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-queue-deferred.h'];

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_queue_deferred.h',
    fs.readFileSync(new URL('./kosmickrisp-queue-deferred.h', import.meta.url), 'utf8'));

  edit('src/vulkan/runtime/vk_queue.h', s => s
    .replace('   struct {\n      /** Current submit mode',
      `   /* Valid only while driver_submit is executing under queue locking. */
   struct vk_queue_submit *driver_submit_active;

   struct {
      /** Current submit mode`)
    .replace('      struct list_head submits;',
      `      struct list_head submits;
      /* Prevent a newly created worker from overtaking the immediate submit
       * which owns a registered continuation but has not returned yet. */
      struct vk_queue_submit *driver_handoff;`)
    .replace('   bool _has_binary_permanent_semaphore_wait;',
      `   /* A driver may retain this exact submit while a host continuation
    * prepares its remaining work. Point 1 must become complete before retry. */
   struct vk_sync *driver_defer_sync;
   bool _driver_unwrapped;

   bool _has_binary_permanent_semaphore_wait;`));

  edit('src/vulkan/runtime/vk_queue.c', s => s
    .replace(`      if (semaphore->type == VK_SEMAPHORE_TYPE_BINARY) {
         if (vk_device_supports_threaded_submit(queue->base.device))`,
      `      struct vk_sync_binary *binary = vk_sync_as_binary(&semaphore->permanent);
      const bool native_binary = binary &&
         (binary->timeline.type->features & VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL);
      if (semaphore->type == VK_SEMAPHORE_TYPE_BINARY && !native_binary) {
         if (vk_device_supports_threaded_submit(queue->base.device))`)
    .replace(`   submit->waits[submit->wait_count] = (struct vk_sync_wait) {`,
      `   /* Snapshot a native timeline-backed binary point at API submission.
    * A queued producer may not have reached driver_submit yet. Keeping the
    * native payload and reserved value avoids a blocking binary-payload move
    * in vkQueueSubmit while that producer has a host continuation. */
   struct vk_sync_binary *binary = vk_sync_as_binary(sync);
   if (binary &&
       (binary->timeline.type->features & VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL)) {
      sync = &binary->timeline;
      wait_value = binary->next_point;
   }

   submit->waits[submit->wait_count] = (struct vk_sync_wait) {`)
    .replace(`   submit->signals[submit->signal_count] = (struct vk_sync_signal) {`,
      `   struct vk_sync_binary *binary = vk_sync_as_binary(sync);
   if (binary &&
       (binary->timeline.type->features & VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL)) {
      sync = &binary->timeline;
      signal_value = ++binary->next_point;
   }

   submit->signals[submit->signal_count] = (struct vk_sync_signal) {`)
    .replace(`   uint32_t wait_count = 0;
   for (uint32_t i = 0; i < submit->wait_count; i++) {`,
      `   if (!submit->_driver_unwrapped) {
   uint32_t wait_count = 0;
   for (uint32_t i = 0; i < submit->wait_count; i++) {`)
    .replace(`   /* For queues participating in emulation, redirect the submission to the`,
      `   submit->_driver_unwrapped = true;
   }

   /* For queues participating in emulation, redirect the submission to the`)
    .replace(`      result = real_queue->driver_submit(real_queue, submit);`,
      `      real_queue->driver_submit_active = submit;
      result = real_queue->driver_submit(real_queue, submit);
      real_queue->driver_submit_active = NULL;`)
    .replace(`      result = queue->driver_submit(queue, submit);`,
      `      queue->driver_submit_active = submit;
      result = queue->driver_submit(queue, submit);
      queue->driver_submit_active = NULL;`)
    .replace(`      result = vk_sync_wait_many(queue->base.device,
                                 submit->wait_count, submit->waits,
                                 VK_SYNC_WAIT_PENDING, UINT64_MAX);`,
      `      /* Encoding may take seconds. Keep the unresolved dependency on
       * this CPU worker, rather than in an already committed GPU command. */
      if (submit->driver_defer_sync) {
         result = vk_sync_wait(queue->base.device, submit->driver_defer_sync,
                               1, 0, UINT64_MAX);
         if (unlikely(result != VK_SUCCESS)) {
            vk_queue_set_lost(queue, "Host continuation wait failed");
            return 1;
         }
         submit->driver_defer_sync = NULL;
      }

      result = vk_sync_wait_many(queue->base.device,
                                 submit->wait_count, submit->waits,
                                 VK_SYNC_WAIT_PENDING, UINT64_MAX);`)
    .replace('      if (list_is_empty(&queue->submit.submits)) {',
      `      if (queue->submit.driver_handoff ||
          list_is_empty(&queue->submit.submits)) {`)
    .replace(`      result = vk_queue_submit_final(queue, submit);
      if (unlikely(result != VK_SUCCESS)) {
         vk_queue_set_lost(queue, "queue::driver_submit failed");`,
      `      result = vk_queue_submit_final(queue, submit);
      if (result == VK_NOT_READY && submit->driver_defer_sync) {
         mtx_lock(&queue->submit.mutex);
         continue;
      }
      if (unlikely(result != VK_SUCCESS)) {
         vk_queue_set_lost(queue, "queue::driver_submit failed");`)
    .replace(`   assert(list_is_empty(&queue->submit.submits));
   queue->submit.mode = VK_QUEUE_SUBMIT_MODE_IMMEDIATE;`,
      `   assert(list_is_empty(&queue->submit.submits) ||
          vk_device_is_lost_no_report(queue->base.device));
   queue->submit.mode = VK_QUEUE_SUBMIT_MODE_IMMEDIATE;`)
    .replace(`      result = vk_queue_submit_final(queue, submit);
      if (unlikely(result != VK_SUCCESS))
         goto fail;

      /* If threaded submit is possible`,
      `      result = vk_queue_submit_final(queue, submit);
      if (result == VK_NOT_READY &&
          queue->submit.mode == VK_QUEUE_SUBMIT_MODE_THREADED) {
         assert(queue->submit.mode == VK_QUEUE_SUBMIT_MODE_THREADED);
         mtx_lock(&queue->submit.mutex);
         if (queue->submit.driver_handoff == submit) {
            list_add(&submit->link, &queue->submit.submits);
            queue->submit.driver_handoff = NULL;
         } else {
            list_addtail(&submit->link, &queue->submit.submits);
         }
         cnd_signal(&queue->submit.push);
         mtx_unlock(&queue->submit.mutex);
         return VK_SUCCESS;
      }
      if (unlikely(result != VK_SUCCESS))
         goto fail;

      /* If threaded submit is possible`));

  edit('src/kosmickrisp/vulkan/kk_sync.h', s => s
    .replace('#include "vk_sync.h"', '#include "vk_sync.h"\n#include "util/u_atomic.h"\n#include "c11/threads.h"')
    .replace('   mtl_shared_event *mtl_handle;',
      `   mtl_shared_event *mtl_handle;
   /* Highest value whose GPU signal has actually been committed. */
   uint64_t pending_value;
   /* Completion queries must not free this sync while its signal publisher
    * still accesses it after the GPU may have completed the native signal. */
   mtx_t publish_mutex;`)
    .replace('extern const struct vk_sync_type kk_sync_type;',
      `static inline void
kk_sync_mark_pending(struct kk_sync_timeline *sync, uint64_t value)
{
   uint64_t old = p_atomic_read(&sync->pending_value);
   while (old < value) {
      uint64_t previous = p_atomic_cmpxchg(&sync->pending_value, old, value);
      if (previous == old)
         break;
      old = previous;
   }
}

extern const struct vk_sync_type kk_sync_type;`));

  edit('src/kosmickrisp/vulkan/kk_sync.c', s => s
    .replace('   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);',
      `   if (mtx_init(&timeline->publish_mutex, mtx_plain) != thrd_success)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);
   if (!timeline->mtl_handle) {
      mtx_destroy(&timeline->publish_mutex);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   p_atomic_set(&timeline->pending_value, initial_value);`)
    .replace('   mtl_release(timeline->mtl_handle);',
      '   mtl_release(timeline->mtl_handle);\n   mtx_destroy(&timeline->publish_mutex);')
    .replace('   mtl_shared_event_set_signaled_value(timeline->mtl_handle, value);',
      `   mtx_lock(&timeline->publish_mutex);
   mtl_shared_event_set_signaled_value(timeline->mtl_handle, value);
   kk_sync_mark_pending(timeline, value);
   mtx_unlock(&timeline->publish_mutex);`)
    .replace('   *value = mtl_shared_event_get_signaled_value(timeline->mtl_handle);',
      `   mtx_lock(&timeline->publish_mutex);
   *value = mtl_shared_event_get_signaled_value(timeline->mtl_handle);
   mtx_unlock(&timeline->publish_mutex);`)
    .replace(`   return VK_SUCCESS;
}

static VkResult
kk_timeline_wait`,
      `   return vk_device_is_lost(device) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult
kk_timeline_wait`)
    .replace(`   /* abs_timeout_ns is the point in time`,
      `   if (wait_flags & VK_SYNC_WAIT_PENDING) {
      for (;;) {
         if (vk_device_is_lost(device))
            return VK_ERROR_DEVICE_LOST;
         if (p_atomic_read(&timeline->pending_value) >= wait_value)
            return VK_SUCCESS;
         if (abs_timeout_ns != OS_TIMEOUT_INFINITE &&
             os_time_get_nano() >= abs_timeout_ns)
            return VK_TIMEOUT;
         os_time_sleep(1000);
      }
   }

   /* abs_timeout_ns is the point in time`)
    .replace(`   int completed = mtl_shared_event_wait_until_signaled_value(
      timeline->mtl_handle, wait_value, timeout_ms);

   return completed != 0 ? VK_SUCCESS : VK_TIMEOUT;`,
      `   for (;;) {
      if (vk_device_is_lost(device))
         return VK_ERROR_DEVICE_LOST;
      int completed = mtl_shared_event_wait_until_signaled_value(
         timeline->mtl_handle, wait_value, MIN2(timeout_ms, 50));
      if (completed) {
         mtx_lock(&timeline->publish_mutex);
         mtx_unlock(&timeline->publish_mutex);
         return vk_device_is_lost(device) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
      }
      if (abs_timeout_ns != OS_TIMEOUT_INFINITE) {
         uint64_t now = os_time_get_nano();
         if (now >= abs_timeout_ns)
            return VK_TIMEOUT;
         timeout_ms = (abs_timeout_ns - now + 999999) / 1000000;
      }
   }`));

  edit('src/kosmickrisp/vulkan/kk_device.c', s => s
    .replace('   /* The original extension has no feature structure.',
      `   /* Metal cannot order a queue behind work that has not been encoded.
    * Start Mesa's submit worker only when a pending dependency requires it. */
   vk_device_enable_threaded_submit(&dev->vk);

   /* The original extension has no feature structure.`)
    .replace(`   /* Meta first since it may destroy Vulkan objects */`,
      `   /* Host continuations may still encode using shared meta resources.
    * Join/cancel submit workers and drain their native callbacks first. */
   while (dev->queue_count > 0)
      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);

   /* Meta first since it may destroy Vulkan objects */`)
    .replace(`   while (dev->queue_count > 0)
      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);

   kk_sparse_device_finish(dev);`, '   kk_sparse_device_finish(dev);'));

  edit('src/kosmickrisp/vulkan/kk_queue.h', s => s
    .replace('   cnd_t cond;',
      `   cnd_t cond;

   struct {
      struct vk_sync *ready;
      struct vk_queue_submit *submit;
      VkResult (*resume)(struct kk_queue *, void *, bool);
      void *data;
      uint32_t next_command;
   } deferred;`)
    .replace('void kk_queue_finish(struct kk_device *dev, struct kk_queue *queue);',
      `void kk_queue_finish(struct kk_device *dev, struct kk_queue *queue);

/* Register after CPU preflight, before capture. ready is a fresh timeline
 * point, initially zero, completed at value 1. Success transfers one off-thread
 * resume/cancel callback; failure none. All later failures use device loss. */
VkResult kk_queue_defer(struct kk_queue *queue, struct vk_sync *ready,
   VkResult (*resume)(struct kk_queue *, void *, bool), void *data);`));

  edit('src/kosmickrisp/vulkan/kk_queue.c', s => s
    .replace('struct kk_commit_data {', '#include "kk_queue_deferred.h"\n\nstruct kk_commit_data {')
    .replace(`   for (struct vk_sync_wait *wait = submit->waits,`,
      `   /* An internally synchronized submit may already have selected the
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
   for (struct vk_sync_wait *wait = submit->waits,`)
    .replace(`   for (uint32_t i = 0; i < submit->command_buffer_count; ++i) {`,
      `   }
   for (uint32_t i = first_command; i < submit->command_buffer_count; ++i) {`)
    .replace(`         i += consumed - 1;
         continue;`,
      `         i += consumed - 1;
         if (queue->deferred.resume) {
            queue->deferred.next_command = i + 1;
            return VK_NOT_READY;
         }
         continue;`)
    .replace(`      cmd_buffer->submitted = true;

   }`,
      `      cmd_buffer->submitted = true;
      if (queue->deferred.resume) {
         queue->deferred.next_command = i + 1;
         return VK_NOT_READY;
      }

   }`)
    .replace(`      mtl_signal_event(queue->mtl_handle, sync->mtl_handle,
                       signal->signal_value);`,
      `      mtx_lock(&sync->publish_mutex);
      mtl_signal_event(queue->mtl_handle, sync->mtl_handle,
                       signal->signal_value);
      kk_sync_mark_pending(sync, signal->signal_value);
      mtx_unlock(&sync->publish_mutex);`)
    .replace(`   /* There may be in-flight command buffers, wait for them to finish. */`,
      `   /* Join the Mesa worker before freeing its command pools or the
    * caller's deferred payload. Lost waits terminate through vk_sync. */
   vk_queue_finish(&queue->vk);
   if (queue->deferred.resume)
      (void)kk_queue_resume_deferred(queue, true);

   /* There may be in-flight command buffers, wait for them to finish. */`)
    .replace(`   mtx_destroy(&queue->mutex);

   vk_queue_finish(&queue->vk);`, '   mtx_destroy(&queue->mutex);'));
}
