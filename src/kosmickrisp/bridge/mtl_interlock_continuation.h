/* SPDX-License-Identifier: MIT */
#ifndef MTL_INTERLOCK_CONTINUATION_H
#define MTL_INTERLOCK_CONTINUATION_H

#include "mtl_types.h"
#include <stdbool.h>
#include <stdint.h>

struct mtl_interlock_context;
typedef bool (*mtl_interlock_encode_callback)(void *, mtl_command_queue *,
   mtl_feedback_handler_callback, void *);
struct mtl_interlock_segment {
   mtl_command_buffer *capture;
   void *data;
   /* Success transfers one completion callback; false commits nothing and
    * transfers no callback.
    * The encoder makes newly created resources resident before committing. */
   mtl_interlock_encode_callback encode;
   /* Optional paired hooks keep recorded data alive through the final
    * callback, including command-pool reset after an application fence. */
   void (*retain)(void *);
   void (*release)(void *);
};

struct mtl_interlock_context *mtl_interlock_context_create(
   mtl_device *device, mtl_command_queue *application, mtl_residency_set *residency,
   void (*lost)(void *, const char *), void *lost_data);
/* One captured segment and its auxiliary work. False commits no GPU work
 * and transfers no callback. True transfers one completion after capture and
 * accepted auxiliary work drain, including cancellation and late failure.
 * The shared queue continuation owns the next segment or application tail. */
bool mtl_interlock_run_segment(struct mtl_interlock_context *context,
   const struct mtl_interlock_segment *segment,
   mtl_feedback_handler_callback completed, void *data);
/* Ended command-buffer parts preceding capture retain their recording order.
 * The final command must be segment->capture. The same ownership contract
 * applies to the complete batch, and no false return commits any part. */
bool mtl_interlock_run_segment_commands(struct mtl_interlock_context *context,
   const struct mtl_interlock_segment *segment,
   mtl_command_buffer *const *commands, uint32_t command_count,
   mtl_feedback_handler_callback completed, void *data);
void mtl_interlock_complete_async(struct mtl_interlock_context *context,
   mtl_feedback_handler_callback completed, void *data, int error);
void mtl_interlock_context_cancel(struct mtl_interlock_context *context);
void mtl_interlock_context_drain(struct mtl_interlock_context *context);
void mtl_interlock_context_finish(struct mtl_interlock_context *context);
bool mtl_interlock_default_sample_positions(mtl_device *device,
   struct mtl_sample_position *positions, uint32_t count);

#endif
