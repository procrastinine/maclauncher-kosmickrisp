/* SPDX-License-Identifier: MIT */
#ifndef KK_HOST_DEFERRED_SUBMIT_IMPL_H
#define KK_HOST_DEFERRED_SUBMIT_IMPL_H

#include "vk_sync.h"

VkResult
kk_queue_defer(struct kk_queue *queue, struct vk_sync *ready,
   VkResult (*resume)(struct kk_queue *, void *, bool), void *data)
{
   struct kk_device *dev = kk_queue_device(queue);
   struct vk_queue_submit *submit = queue->vk.driver_submit_active;
   uint64_t value;
   if (!submit || !ready || !resume || queue->deferred.resume ||
       !(ready->flags & VK_SYNC_IS_TIMELINE) ||
       vk_device_is_lost(&dev->vk))
      return VK_ERROR_DEVICE_LOST;
   VkResult result = vk_sync_get_value(&dev->vk, ready, &value);
   if (result != VK_SUCCESS || value != 0)
      return VK_ERROR_DEVICE_LOST;
   const bool handoff = queue->vk.submit.mode == VK_QUEUE_SUBMIT_MODE_IMMEDIATE;
   if (handoff) {
      mtx_lock(&queue->vk.submit.mutex);
      queue->vk.submit.driver_handoff = submit;
      mtx_unlock(&queue->vk.submit.mutex);
   }
   result = vk_queue_enable_submit_thread(&queue->vk);
   if (result != VK_SUCCESS) {
      if (handoff) {
         mtx_lock(&queue->vk.submit.mutex);
         queue->vk.submit.driver_handoff = NULL;
         mtx_unlock(&queue->vk.submit.mutex);
      }
      return result;
   }

   /* The caller owns this fresh point until its one resume/cancel callback.
    * The worker waits on the host; no unresolved GPU wait is submitted. */
   queue->deferred.ready = ready;
   queue->deferred.resume = resume;
   queue->deferred.data = data;
   queue->deferred.submit = submit;
   submit->driver_defer_sync = ready;
   return VK_SUCCESS;
}

static VkResult
kk_queue_resume_deferred(struct kk_queue *queue, bool cancelled)
{
   VkResult (*resume)(struct kk_queue *, void *, bool) = queue->deferred.resume;
   void *data = queue->deferred.data;
   queue->deferred.ready = NULL;
   queue->deferred.resume = NULL;
   queue->deferred.data = NULL;
   queue->deferred.submit = NULL;
   /* Clear ownership before invoking the callback: a resumed segment may
    * register another fresh point. This is never called with queue->mutex. */
   VkResult result = resume(queue, data, cancelled);
   if (result != VK_SUCCESS)
      return vk_device_set_lost(&kk_queue_device(queue)->vk,
                                "Host-deferred continuation failed");
   return VK_SUCCESS;
}

#endif
