/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SUBMIT_H
#define KK_INTERLOCK_SUBMIT_H

#include "kosmickrisp/bridge/mtl_interlock_continuation.h"
#include "vk_sync.h"
#include "util/list.h"

struct kk_device;
struct kk_queue;
struct kk_cmd_buffer;
struct kk_sync_timeline;
struct kk_interlock_registry;
struct kk_interlock_queue;
struct kk_interlock_heap;

/* The packet's data belongs to its recorded command buffer until the final
 * commit callback. Reset releases it only after every continuation callback. */
struct kk_interlock_segment {
   struct kk_interlock_segment *next;
   /* Only the recording head uses tail; ordinary command buffers need no slot. */
   struct kk_interlock_segment *tail;
   struct kk_device *device;
   uint32_t references;
   struct kk_interlock_heap *heap;
   void *data;
   mtl_interlock_encode_callback encode;
   struct mtl_interlock_segment native;
   mtl_command_buffer **commands;
   uint32_t command_count;
   void (*finish)(struct kk_device *, void *);
};

VkResult kk_interlock_submission_init(struct kk_device *dev);
VkResult kk_interlock_submission_lost(struct kk_device *dev);
void kk_interlock_submission_drain(struct kk_device *dev);
void kk_interlock_submission_finish(struct kk_device *dev);
VkResult kk_interlock_commit(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
   mtl_feedback_handler_callback callback, void *data);
void kk_interlock_queue_finish(struct kk_queue *queue);
void kk_interlock_segments_finish(struct kk_cmd_buffer *cmd);
/* Success takes the recording owner of data; failure leaves it with caller. */
VkResult kk_interlock_split(struct kk_cmd_buffer *cmd, void *data,
   mtl_interlock_encode_callback encode,
   void (*finish)(struct kk_device *, void *));
void kk_interlock_sync_init(struct kk_device *dev, struct kk_sync_timeline *sync);
void kk_interlock_sync_finish(struct kk_device *dev, struct kk_sync_timeline *sync);

#endif
