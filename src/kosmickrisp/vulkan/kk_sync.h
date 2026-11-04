/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_SYNC_TYPES_H
#define KK_SYNC_TYPES_H 1

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_sync.h"
#include "util/list.h"
#include "util/u_atomic.h"
#include "c11/threads.h"

struct kk_queue;
struct kk_device;

struct kk_sync_timeline {
   struct vk_sync base;
   mtl_shared_event *mtl_handle;
   struct list_head interlock_link;
   bool interlock_registered;
   /* Highest value whose GPU signal has actually been committed. */
   uint64_t pending_value;
   /* Completion queries must not free this sync while its signal publisher
    * still accesses it after the GPU may have completed the native signal. */
   mtx_t publish_mutex;
   /* Shared opaque payloads can outlive their creating device. Only blocked
    * calls register their current device for publication notification. */
   mtx_t pending_mutex;
   struct list_head pending_waits;
   uint32_t pending_waiters;
};

VkResult kk_sync_device_init(struct kk_device *dev);
void kk_sync_device_finish(struct kk_device *dev);
VkResult kk_sync_pending_init(struct kk_sync_timeline *sync);
void kk_sync_pending_finish(struct kk_sync_timeline *sync);
void kk_sync_notify_pending(struct kk_sync_timeline *sync);

static inline void
kk_sync_mark_pending(struct kk_sync_timeline *sync, uint64_t value)
{
   uint64_t old = p_atomic_read(&sync->pending_value);
   while (old < value) {
      uint64_t previous = p_atomic_cmpxchg(&sync->pending_value, old, value);
      if (previous == old) {
         /* The caller already owns publish_mutex. Only blocked publication
          * waits need the separate condition lock/notification. */
         if (p_atomic_read(&sync->pending_waiters))
            kk_sync_notify_pending(sync);
         break;
      }
      old = previous;
   }
}

extern const struct vk_sync_type kk_sync_type;

#endif
