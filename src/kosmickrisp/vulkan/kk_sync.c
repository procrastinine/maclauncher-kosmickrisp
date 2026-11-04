/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_sync.h"

#include "kk_device.h"
#include "kk_interlock_submit.h"
#include "kk_perf.h"
#include "vk_common_entrypoints.h"

#include "kosmickrisp/bridge/mtl_bridge.h"

static VkResult
kk_timeline_init(struct vk_device *device, struct vk_sync *sync,
                 uint64_t initial_value)
{
   struct kk_sync_timeline *timeline =
      container_of(sync, struct kk_sync_timeline, base);

   struct kk_device *dev = container_of(device, struct kk_device, vk);
   if (mtx_init(&timeline->publish_mutex, mtx_plain) != thrd_success)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);
   if (!timeline->mtl_handle)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (!timeline->mtl_handle) {
      mtx_destroy(&timeline->publish_mutex);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   p_atomic_set(&timeline->pending_value, initial_value);
   mtl_shared_event_set_signaled_value(timeline->mtl_handle, initial_value);
   kk_interlock_sync_init(dev, timeline);

   return VK_SUCCESS;
}

static void
kk_timeline_finish(struct vk_device *device, struct vk_sync *sync)
{
   struct kk_sync_timeline *timeline =
      container_of(sync, struct kk_sync_timeline, base);
   kk_interlock_sync_finish(container_of(device, struct kk_device, vk), timeline);
   mtl_release(timeline->mtl_handle);
   mtx_destroy(&timeline->publish_mutex);
}

static VkResult
kk_timeline_signal(struct vk_device *device, struct vk_sync *sync,
                   uint64_t value)
{
   struct kk_sync_timeline *timeline =
      container_of(sync, struct kk_sync_timeline, base);
   mtx_lock(&timeline->publish_mutex);
   mtl_shared_event_set_signaled_value(timeline->mtl_handle, value);
   kk_sync_mark_pending(timeline, value);
   mtx_unlock(&timeline->publish_mutex);
   return vk_device_is_lost(device) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult
kk_timeline_get_value(struct vk_device *device, struct vk_sync *sync,
                      uint64_t *value)
{
   struct kk_sync_timeline *timeline =
      container_of(sync, struct kk_sync_timeline, base);
   mtx_lock(&timeline->publish_mutex);
   *value = mtl_shared_event_get_signaled_value(timeline->mtl_handle);
   mtx_unlock(&timeline->publish_mutex);
   return vk_device_is_lost(device) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult
kk_timeline_wait_impl(struct vk_device *device, struct vk_sync *sync,
                 uint64_t wait_value, enum vk_sync_wait_flags wait_flags,
                 uint64_t abs_timeout_ns)
{
   struct kk_sync_timeline *timeline =
      container_of(sync, struct kk_sync_timeline, base);

   if (wait_flags & VK_SYNC_WAIT_PENDING) {
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

   /* abs_timeout_ns is the point in time when we should stop waiting, not the
    * absolute time in ns. Therefore, we need to compute the delta from now to
    * when we should stop waiting and convert to ms for Metal to be happy
    * (Similar to what dzn does).
    */
   uint64_t timeout_ms = 0u;
   if (abs_timeout_ns == OS_TIMEOUT_INFINITE) {
      timeout_ms = OS_TIMEOUT_INFINITE;
   } else {
      uint64_t cur_time = os_time_get_nano();
      uint64_t rel_timeout_ns =
         abs_timeout_ns > cur_time ? abs_timeout_ns - cur_time : 0;

      timeout_ms =
         (rel_timeout_ns / 1000000) + (rel_timeout_ns % 1000000 ? 1 : 0);
   }
   for (;;) {
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
   }
}

/* The backend metric includes internal pending waits as well as Metal event
 * waits; API fence/semaphore metrics below are enclosing, potentially nested
 * latencies rather than disjoint CPU work. */
static VkResult
kk_timeline_wait(struct vk_device *device, struct vk_sync *sync,
                 uint64_t wait_value, enum vk_sync_wait_flags wait_flags,
                 uint64_t abs_timeout_ns)
{
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   return KK_PERF_CALL(dev, KK_PERF_SYNC_WAIT,
      kk_timeline_wait_impl(device, sync, wait_value, wait_flags, abs_timeout_ns));
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_wait_for_fences(VkDevice device, uint32_t count, const VkFence *fences,
                        VkBool32 wait_all, uint64_t timeout)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   return KK_PERF_CALL(dev, KK_PERF_FENCE_WAIT,
      vk_common_WaitForFences(device, count, fences, wait_all, timeout));
}

static VKAPI_ATTR VkResult VKAPI_CALL
kk_perf_wait_semaphores(VkDevice device, const VkSemaphoreWaitInfo *info,
                        uint64_t timeout)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   return KK_PERF_CALL(dev, KK_PERF_SEMAPHORE_WAIT,
      vk_common_WaitSemaphores(device, info, timeout));
}

void
kk_perf_init_wait_dispatch(struct kk_device *dev)
{
   dev->vk.dispatch_table.WaitForFences = kk_perf_wait_for_fences;
   dev->vk.dispatch_table.WaitSemaphores = kk_perf_wait_semaphores;
   dev->exposed_dispatch_table.WaitForFences = kk_perf_wait_for_fences;
   dev->exposed_dispatch_table.WaitSemaphores = kk_perf_wait_semaphores;
}

const struct vk_sync_type kk_sync_type = {
   .size = sizeof(struct kk_sync_timeline),
   .features = VK_SYNC_FEATURE_TIMELINE | VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING |
               VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init = kk_timeline_init,
   .finish = kk_timeline_finish,
   .signal = kk_timeline_signal,
   .get_value = kk_timeline_get_value,
   .reset = NULL,
   .move = NULL,
   .wait = kk_timeline_wait,
   .wait_many = NULL,
   .import_opaque_fd = NULL,
   .export_opaque_fd = NULL,
   .import_sync_file = NULL,
   .export_sync_file = NULL,
   .import_win32_handle = NULL,
   .export_win32_handle = NULL,
   .set_win32_export_params = NULL,
};
