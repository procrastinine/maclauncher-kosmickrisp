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
};

static inline void
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

extern const struct vk_sync_type kk_sync_type;

#endif
