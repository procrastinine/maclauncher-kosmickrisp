/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_QUEUE_H
#define KK_QUEUE_H 1

#include "kk_private.h"

#include "kk_cmd_pool.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_queue.h"

#include "c11/threads.h"

struct kk_queue {
   struct vk_queue vk;
   struct mtl_command_queue *mtl_handle;
   /* Immutable setup-selected callback, retained by actual-use adapters. */
   VkResult (*native_submit)(struct vk_queue *, struct vk_queue_submit *);
   struct kk_interlock_queue *interlock_submission;
   /* Classic queue and event of forwarded temporal frames, made at the first. */
   void *temporal_queue, *temporal_event;
   uint64_t temporal_value;

   /* Resources for command buffer re-recording. These are required for
    * SIMULTANEOUS_USE command buffers where the same command buffer can be
    * submitted multiple times at the same time. Metal's command buffers are a
    * one time use, so the queue will own the re-recorded ones. */
   struct kk_cmd_pool *cmd_pool;
   struct util_dynarray free_cmd_buffers;
   uint32_t commits_in_flight;
   mtx_t mutex;
   cnd_t cond;

   struct {
      struct vk_sync *ready;
      struct vk_queue_submit *submit;
      VkResult (*resume)(struct kk_queue *, void *, bool);
      void *data;
      uint32_t next_command;
      uint64_t perf_queued_ns;
   } deferred;
};

static inline struct kk_device *
kk_queue_device(struct kk_queue *queue)
{
   return (struct kk_device *)queue->vk.base.device;
}

VkResult kk_queue_init(struct kk_device *dev, struct kk_queue *queue,
                       const VkDeviceQueueCreateInfo *pCreateInfo,
                       uint32_t index_in_family);

void kk_queue_finish(struct kk_device *dev, struct kk_queue *queue);


/* Register after CPU preflight, before capture. ready is a fresh timeline
 * point, initially zero, completed at value 1. Success transfers one off-thread
 * resume/cancel callback; failure none. All later failures use device loss. */
VkResult kk_queue_defer(struct kk_queue *queue, struct vk_sync *ready,
   VkResult (*resume)(struct kk_queue *, void *, bool), void *data);

#endif
