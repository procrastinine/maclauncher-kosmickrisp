/* SPDX-License-Identifier: MIT */
#ifndef KK_RECORDED_SCENE_H
#define KK_RECORDED_SCENE_H
#include <stdbool.h>
#include "scene/scene-interop.h"
#include "kosmickrisp/bridge/mtl_types.h"
struct kk_cmd_buffer;
struct kk_queue;
struct kk_scene_record;
struct kk_scene_flight;
void kk_scene_recording_failed(struct kk_cmd_buffer *cmd);
VkResult kk_scene_submission_reserve(struct kk_queue *, struct kk_cmd_buffer *);
void kk_scene_reset(struct kk_cmd_buffer *cmd);
struct kk_scene_submission;
VkResult kk_scene_submission_begin(struct kk_queue *queue, struct kk_cmd_buffer *cmd,
   mtl_feedback_handler_callback callback, void *data,
   struct kk_scene_submission **submission);
void kk_scene_submission_feedback(struct mtl_feedback_data *data);
void kk_scene_submission_failed(struct kk_scene_submission *submission);
void kk_scene_submission_committed(struct kk_scene_submission *, struct kk_queue *);
struct kk_scene_submission *kk_scene_submission_acquire(mtl_feedback_handler_callback, void *);
void kk_scene_submission_release(struct kk_scene_submission *);
#endif
