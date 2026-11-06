/* SPDX-License-Identifier: MIT */
#include "../mtl_interlock_continuation.h"
struct mtl_interlock_context *
mtl_interlock_context_create(mtl_device *dev, mtl_command_queue *queue,
   mtl_residency_set *set, void (*lost)(void *, const char *), void *data)
{
   return NULL;
}
void mtl_interlock_context_cancel(struct mtl_interlock_context *context) {}
void mtl_interlock_context_drain(struct mtl_interlock_context *context) {}
void mtl_interlock_context_finish(struct mtl_interlock_context *context) {}
bool mtl_interlock_run_segment(struct mtl_interlock_context *context,
   const struct mtl_interlock_segment *segment,
   mtl_feedback_handler_callback callback, void *data) { return false; }
bool mtl_interlock_run_segment_commands(struct mtl_interlock_context *context,
   const struct mtl_interlock_segment *segment,
   mtl_command_buffer *const *commands, uint32_t command_count,
   mtl_feedback_handler_callback callback, void *data) { return false; }
void mtl_interlock_complete_async(struct mtl_interlock_context *context,
   mtl_feedback_handler_callback callback, void *data, int error) {}
bool mtl_interlock_default_sample_positions(mtl_device *device,
   struct mtl_sample_position *positions, uint32_t count) { return false; }
