/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef MTL_COMMAND_QUEUE_H
#define MTL_COMMAND_QUEUE_H 1

#include "mtl_types.h"

#include <stdint.h>

/* MTL4CommitOptions */
mtl_commit_options *mtl_new_commit_options(void);
void
mtl_commit_options_add_feedback_handler(mtl_commit_options *options,
                                        mtl_feedback_handler_callback callback,
                                        void *data);

typedef void (*mtl_gpu_range_callback)(void *data, uint64_t frame,
                                       double start, double end,
                                       uint32_t command_buffers);
void mtl_commit_options_add_tracked_feedback_handler(
   mtl_commit_options *options, mtl_feedback_handler_callback callback,
   void *data, mtl_gpu_range_callback gpu_callback, void *gpu_data,
   uint64_t frame, uint32_t command_buffers);

/* MTL4CommandQueue */
mtl_command_queue *mtl_new_command_queue(mtl_device *device);

void mtl_command_queue_add_residency_set(mtl_command_queue *cmd_queue,
                                         mtl_residency_set *set);

void mtl_command_queue_remove_residency_set(mtl_command_queue *cmd_queue,
                                            mtl_residency_set *set);

void mtl_signal_event(mtl_command_queue *queue, mtl_event *event,
                      uint64_t value);
void mtl_wait_for_event(mtl_command_queue *queue, mtl_event *event,
                        uint64_t value);

void mtl_command_queue_commit(mtl_command_queue *queue,
                              mtl_command_buffer **command_buffers,
                              uint32_t count, mtl_commit_options *options);

void mtl_command_queue_wait_for_drawable(mtl_command_queue *queue,
                                         void *drawable);

void mtl_command_queue_signal_drawable(mtl_command_queue *queue,
                                       void *drawable);

#endif /* MTL_COMMAND_QUEUE_H */