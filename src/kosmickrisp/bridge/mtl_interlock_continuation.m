#include "mtl_gpu_timing.h"
/* SPDX-License-Identifier: MIT */
#include "mtl_interlock_continuation.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool
mtl_interlock_default_sample_positions(mtl_device *handle,
   struct mtl_sample_position *positions, uint32_t count)
{
   id<MTLDevice> device = (__bridge id<MTLDevice>)handle;
   _Static_assert(sizeof(*positions) == sizeof(MTLSamplePosition),
      "Sample-position bridge layout");
   if (![device supportsTextureSampleCount:count])
      return false;
   [device getDefaultSamplePositions:(MTLSamplePosition *)positions count:count];
   return true;
}

struct mtl_interlock_context {
   id<MTL4CommandQueue> application;
   id<MTL4CommandQueue> auxiliary;
   id<MTLResidencySet> residency;
   id<MTLSharedEvent> capture_ready;
   MTLSharedEventListener *listener;
   dispatch_group_t pending;
   dispatch_queue_t completion_queue;
   bool residency_attached;
   bool trace;
   atomic_bool cancelled;
   uint64_t next_capture;
   void (*lost)(void *, const char *);
   void *lost_data;
};

struct mtl_interlock_packet {
   struct mtl_interlock_context *context;
   struct mtl_interlock_segment segment;
   id<MTL4CommandBuffer> *commands;
   uint32_t command_count;
   uint64_t capture_value;
   atomic_uint references;
   atomic_int error;
   mtl_feedback_handler_callback completed;
   void *data;
};

struct mtl_interlock_context *
mtl_interlock_context_create(mtl_device *device, mtl_command_queue *application,
   mtl_residency_set *residency, void (*lost)(void *, const char *), void *lost_data)
{
   struct mtl_interlock_context *c = calloc(1, sizeof(*c));
   if (!c)
      return NULL;
   const char *debug = getenv("MESA_KK_DEBUG");
   c->trace = debug && strstr(debug, "msl");
   c->application = [(id<MTL4CommandQueue>)application retain];
   c->auxiliary = [(id<MTLDevice>)device newMTL4CommandQueue];
   c->residency = [(id<MTLResidencySet>)residency retain];
   c->capture_ready = [(id<MTLDevice>)device newSharedEvent];
   c->pending = dispatch_group_create();
   c->completion_queue = dispatch_queue_create("kosmickrisp.interlock.complete", NULL);
   if (c->completion_queue)
      c->listener = [[MTLSharedEventListener alloc]
         initWithDispatchQueue:c->completion_queue];
   c->lost = lost;
   c->lost_data = lost_data;
   atomic_init(&c->cancelled, false);
   if (!c->application || !c->auxiliary || !c->capture_ready ||
       !c->listener || !c->pending || !c->completion_queue) {
      mtl_interlock_context_finish(c);
      return NULL;
   }
   if (c->residency) {
      [c->auxiliary addResidencySet:c->residency];
      c->residency_attached = true;
   }
   return c;
}

void
mtl_interlock_context_cancel(struct mtl_interlock_context *c)
{
   if (!c)
      return;
   atomic_store(&c->cancelled, true);
   /* Wake the capture listener on loss, while the capture feedback reference
    * still keeps already-submitted GPU work and its resources alive. */
   c->capture_ready.signaledValue = UINT64_MAX;
}

static void
mtl_interlock_fail(struct mtl_interlock_packet *p, int error)
{
   atomic_store(&p->error, error ? error : MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY);
   bool was_cancelled = atomic_exchange(&p->context->cancelled, true);
   mtl_interlock_context_cancel(p->context);
   if (!was_cancelled && p->context->lost)
      p->context->lost(p->context->lost_data, "Fragment interlock continuation failed");
}

static void
mtl_interlock_packet_release(struct mtl_interlock_packet *p)
{
   if (atomic_fetch_sub(&p->references, 1) != 1)
      return;
   /* Capture feedback, listener, any accepted auxiliary encoding and the
    * starter have all drained. Never invoke completion inline on a caller. */
   dispatch_async(p->context->completion_queue, ^{
      struct mtl_interlock_context *c = p->context;
      int error = atomic_load(&p->error);
      if (!error && atomic_load(&c->cancelled))
         error = MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY;
      struct mtl_feedback_data feedback = {
         .user_data = p->data,
         .error = error,
         .error_message = error ? "Fragment interlock continuation failed" : NULL,
      };
      p->completed(&feedback);
      if (p->segment.release)
         p->segment.release(p->segment.data);
      for (uint32_t i = 0; i < p->command_count; ++i)
         [p->commands[i] release];
      free(p->commands);
      free(p);
      dispatch_group_leave(c->pending);
   });
}

static void
mtl_interlock_auxiliary_done(struct mtl_feedback_data *feedback)
{
   struct mtl_interlock_packet *p = feedback->user_data;
   if (p->context->trace)
      fprintf(stderr, "interlock aux final error=%u capture=%llu\n",
         feedback->error, (unsigned long long)p->capture_value);
   if (feedback->error)
      mtl_interlock_fail(p, feedback->error);
   mtl_interlock_packet_release(p); /* accepted auxiliary completion */
}

static void
mtl_interlock_capture_ready(struct mtl_interlock_packet *p)
{
   @autoreleasepool {
      if (p->context->trace)
         fprintf(stderr, "interlock capture ready value=%llu\n",
            (unsigned long long)p->capture_value);
      if (!atomic_load(&p->context->cancelled)) {
         /* The listener has observed the event signalled after the already
          * committed capture. Preserve ordering on the auxiliary queue. */
         [p->context->auxiliary waitForEvent:p->context->capture_ready
            value:p->capture_value];
         atomic_fetch_add(&p->references, 1);
         if (!p->segment.encode(p->segment.data,
               (void *)p->context->auxiliary, mtl_interlock_auxiliary_done, p)) {
            /* Encoder false commits nothing and transfers no callback.
             * A later error in accepted encoding must use its final callback. */
            mtl_interlock_fail(p, MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY);
            mtl_interlock_packet_release(p);
         }
      }
      mtl_interlock_packet_release(p); /* listener reservation */
   }
}

bool
mtl_interlock_run_segment(struct mtl_interlock_context *c,
   const struct mtl_interlock_segment *segment,
   mtl_feedback_handler_callback completed, void *data)
{
   mtl_command_buffer *capture = segment ? segment->capture : NULL;
   return mtl_interlock_run_segment_commands(c, segment, &capture, 1,
      completed, data);
}

bool
mtl_interlock_run_segment_commands(struct mtl_interlock_context *c,
   const struct mtl_interlock_segment *segment,
   mtl_command_buffer *const *commands, uint32_t command_count,
   mtl_feedback_handler_callback completed, void *data)
{
   if (!c || !segment || !segment->capture || !segment->encode || !completed ||
       !commands || !command_count || commands[command_count - 1] != segment->capture ||
       (!!segment->retain != !!segment->release) ||
       atomic_load(&c->cancelled) || c->next_capture >= UINT64_MAX - 1)
      return false;
   struct mtl_interlock_packet *p = calloc(1, sizeof(*p));
   if (!p)
      return false;
   p->commands = calloc(command_count, sizeof(*p->commands));
   if (!p->commands) {
      free(p);
      return false;
   }
   MTL4CommitOptions *options = [MTL4CommitOptions new];
   if (!options) {
      free(p->commands);
      free(p);
      return false;
   }
   p->context = c;
   p->segment = *segment;
   p->command_count = command_count;
   for (uint32_t i = 0; i < command_count; ++i)
      p->commands[i] = [(id<MTL4CommandBuffer>)commands[i] retain];
   p->capture_value = c->next_capture + 1;
   p->completed = completed;
   p->data = data;
   /* One starter, one capture-feedback and one listener reference. */
   atomic_init(&p->references, 3);
   atomic_init(&p->error, 0);
   NSArray *encoder_samples = __atomic_load_n(&mtl_gpu_timing_enabled, __ATOMIC_ACQUIRE)
      ? mtl_gpu_timing_snapshot(commands, command_count) : nil;
   [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
      if (encoder_samples) mtl_gpu_timing_resolve(encoder_samples, feedback.error == nil);
      if (feedback.error)
         mtl_interlock_fail(p, (int)feedback.error.code);
      mtl_interlock_packet_release(p); /* actual capture completion */
   }];
   [encoder_samples release];
   if (p->segment.retain)
      p->segment.retain(p->segment.data);
   c->next_capture = p->capture_value;
   dispatch_group_enter(c->pending);
   [c->capture_ready notifyListener:c->listener atValue:p->capture_value
      block:^(id<MTLSharedEvent> event, uint64_t value) {
         mtl_interlock_capture_ready(p);
      }];
   /* All false returns precede this first GPU commitment. Every later error
    * retains the one completion until capture plus accepted auxiliary drain. */
   [c->application commit:p->commands count:p->command_count options:options];
   [c->application signalEvent:c->capture_ready value:p->capture_value];
   [options release];
   mtl_interlock_packet_release(p); /* starter reference */
   return true;
}

void
mtl_interlock_context_drain(struct mtl_interlock_context *c)
{
   if (c && c->pending)
      dispatch_group_wait(c->pending, DISPATCH_TIME_FOREVER);
}

void
mtl_interlock_complete_async(struct mtl_interlock_context *c,
   mtl_feedback_handler_callback completed, void *data, int error)
{
   dispatch_group_enter(c->pending);
   dispatch_async(c->completion_queue, ^{
      int effective_error = error ? error : atomic_load(&c->cancelled) ?
         MTL_COMMAND_QUEUE_ERROR_INTERNAL : MTL_COMMAND_QUEUE_ERROR_NONE;
      struct mtl_feedback_data feedback = {
         .user_data = data,
         .error = effective_error,
         .error_message = effective_error ? "Fragment interlock continuation failed" : NULL,
      };
      completed(&feedback);
      dispatch_group_leave(c->pending);
   });
}

void
mtl_interlock_context_finish(struct mtl_interlock_context *c)
{
   if (!c)
      return;
   mtl_interlock_context_drain(c);
   if (c->residency_attached)
      [c->auxiliary removeResidencySet:c->residency];
   [c->application release];
   [c->auxiliary release];
   [c->residency release];
   [c->capture_ready release];
   [c->listener release];
   if (c->pending)
      dispatch_release(c->pending);
   if (c->completion_queue)
      dispatch_release(c->completion_queue);
   free(c);
}
