/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_STACK_H
#define KK_RT_STACK_H
#ifndef KK_RT_ABI_H
#include "kk_rt_abi.h"
#endif
#ifdef __METAL_VERSION__
#define KK_RT_DEVICE device
#define KK_RT_THREAD thread
#define KK_RT_U64 ulong
#define KK_RT_U32 uint
#define KK_RT_ASSERT static_assert
#else
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define KK_RT_DEVICE
#define KK_RT_THREAD
#define KK_RT_U64 uint64_t
#define KK_RT_U32 uint32_t
#define KK_RT_ASSERT _Static_assert
#endif

#define KK_RT_NO_FRAME 0xffffffffffffffffull
#define KK_RT_STOP 0xffffffffu

enum kk_rt_stack_error {
   KK_RT_STACK_OK,
   KK_RT_STACK_RANGE,
   KK_RT_STACK_OVERFLOW,
   KK_RT_STACK_DEPTH,
   KK_RT_STACK_STATE,
};

/* Snapshot before a visible-function invocation. Its outgoing call arguments
 * may then replace the live context without destroying the caller's ray/IO.
 * The common NIR spills follow this header in the same RT-only arena. */
struct kk_rt_cps_frame {
   struct kk_rt_cps_context saved;
   KK_RT_U64 parent;
   KK_RT_U64 previous;
   KK_RT_U64 previous_end;
   KK_RT_U64 end;
   KK_RT_U64 scratch;
   KK_RT_U32 resume_pc;
   KK_RT_U32 trace_depth;
   KK_RT_U32 shader;
   KK_RT_U32 reserved;
   KK_RT_U32 role;
   KK_RT_U32 phase;
   KK_RT_U64 parked;
   KK_RT_U64 trace;
   KK_RT_U64 scope;
};

/* active and top differ while a procedural shader is parked at REPORT. An
 * any-hit activation allocates above top, even though its caller is active. */
struct kk_rt_lane_state {
   struct kk_rt_cps_context context;
   KK_RT_U64 active;
   KK_RT_U64 top;
   KK_RT_U64 end;
   KK_RT_U32 pc;
   KK_RT_U32 error;
   KK_RT_U32 max_trace_depth;
   KK_RT_U32 reserved;
};

KK_RT_ASSERT(sizeof(struct kk_rt_cps_frame) == 544, "CPS frame scalar ABI");
KK_RT_ASSERT(sizeof(struct kk_rt_lane_state) == 496, "CPS lane scalar ABI");
KK_RT_ASSERT(__builtin_offsetof(struct kk_rt_lane_state, context) == 0, "visible context prefix");
KK_RT_ASSERT(__builtin_offsetof(struct kk_rt_cps_frame, scratch) == 488, "CPS scratch offset");

static inline bool
kk_rt_u64_add(KK_RT_U64 a, KK_RT_U64 b, KK_RT_THREAD KK_RT_U64 *out)
{
   if (b > ~(KK_RT_U64)0 - a)
      return false;
   *out = a + b;
   return true;
}

static inline bool
kk_rt_u64_mul(KK_RT_U64 a, KK_RT_U64 b, KK_RT_THREAD KK_RT_U64 *out)
{
   /* Keep the partial products at 32x32 bits. The division-based overflow
    * idiom becomes a 64-bit multiply-with-overflow operation that the Apple8
    * Metal backend cannot legalize. Each sum below is bounded to 34 bits. */
   KK_RT_U32 a_lo = (KK_RT_U32)a, a_hi = (KK_RT_U32)(a >> 32);
   KK_RT_U32 b_lo = (KK_RT_U32)b, b_hi = (KK_RT_U32)(b >> 32);
   if (a_hi && b_hi)
      return false;
   KK_RT_U64 low = (KK_RT_U64)a_lo * b_lo;
   KK_RT_U64 cross0 = (KK_RT_U64)a_hi * b_lo;
   KK_RT_U64 cross1 = (KK_RT_U64)a_lo * b_hi;
   if ((cross0 | cross1) >> 32)
      return false;
   KK_RT_U64 high = (low >> 32) + (KK_RT_U32)cross0 + (KK_RT_U32)cross1;
   if (high >> 32)
      return false;
   *out = (high << 32) | (KK_RT_U32)low;
   return true;
}

/* Launch progress is independent of the completed launch's error state. */
static inline bool
kk_rt_advance_launch(KK_RT_THREAD KK_RT_U64 *launch, KK_RT_U64 count,
                      KK_RT_U32 physical)
{
   if (!physical || *launch >= count || count - *launch <= physical)
      return false;
   *launch += physical;
   return true;
}

static inline bool
kk_rt_stack_fail(KK_RT_DEVICE struct kk_rt_lane_state *lane, KK_RT_U32 error)
{
   if (!lane->error)
      lane->error = error;
   lane->pc = KK_RT_STOP;
   return false;
}

static inline bool
kk_rt_stack_init(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                  struct kk_rt_cps_context context, KK_RT_U32 max_trace_depth)
{
   lane->context = context;
   lane->active = lane->top = KK_RT_NO_FRAME;
   lane->end = 0;
   lane->pc = KK_RT_STOP;
   lane->error = KK_RT_STACK_OK;
   lane->max_trace_depth = max_trace_depth;
   lane->reserved = 0;
   KK_RT_U64 end;
   if (!context.stack_base || (context.stack_base & 15) ||
       !kk_rt_u64_add(context.stack_base, context.stack_size, &end))
      return kk_rt_stack_fail(lane, KK_RT_STACK_RANGE);
   return true;
}

static inline KK_RT_DEVICE struct kk_rt_cps_frame *
kk_rt_stack_frame(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                   KK_RT_DEVICE unsigned char *arena, KK_RT_U64 offset)
{
   if (lane->error || (offset & 15) || lane->end > lane->context.stack_size || offset > lane->end ||
       lane->end - offset < sizeof(struct kk_rt_cps_frame)) {
      kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
      return 0;
   }
   return (KK_RT_DEVICE struct kk_rt_cps_frame *)(arena + offset);
}

static inline bool
kk_rt_stack_enter(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                   KK_RT_DEVICE unsigned char *arena, KK_RT_U32 shader,
                   KK_RT_U32 pc, KK_RT_U64 scratch_size, KK_RT_U32 trace_increment)
{
   if (lane->error || pc == KK_RT_STOP)
      return kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
   KK_RT_U32 depth = 0;
   if (lane->active != KK_RT_NO_FRAME) {
      KK_RT_DEVICE struct kk_rt_cps_frame *parent =
         kk_rt_stack_frame(lane, arena, lane->active);
      if (!parent)
         return false;
      depth = parent->trace_depth;
   }
   if (trace_increment > lane->max_trace_depth ||
       depth > lane->max_trace_depth - trace_increment)
      return kk_rt_stack_fail(lane, KK_RT_STACK_DEPTH);
   KK_RT_U64 scratch, end;
   if ((lane->end & 15) ||
       !kk_rt_u64_add(lane->end, sizeof(struct kk_rt_cps_frame), &scratch) ||
       !kk_rt_u64_add(scratch_size, 15, &end) ||
       !kk_rt_u64_add(scratch, end & ~(KK_RT_U64)15, &end) ||
       end > lane->context.stack_size)
      return kk_rt_stack_fail(lane, KK_RT_STACK_OVERFLOW);

   KK_RT_DEVICE struct kk_rt_cps_frame *frame =
      (KK_RT_DEVICE struct kk_rt_cps_frame *)(arena + lane->end);
   frame->parent = lane->active;
   frame->previous = lane->top;
   frame->previous_end = lane->end;
   frame->end = end;
   frame->scratch = scratch;
   frame->resume_pc = KK_RT_STOP;
   frame->trace_depth = depth + trace_increment;
   frame->shader = shader;
   frame->reserved = 0;
   frame->role = frame->phase = 0;
   frame->parked = frame->trace = KK_RT_NO_FRAME;
   frame->scope = lane->context.child_scope;
   lane->active = lane->top = lane->end;
   lane->end = end;
   lane->pc = pc;
   lane->context.frame_base = scratch;
   frame->saved = lane->context;
   return true;
}

static inline bool
kk_rt_stack_before_shader(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                           KK_RT_DEVICE unsigned char *arena)
{
   KK_RT_DEVICE struct kk_rt_cps_frame *frame =
      kk_rt_stack_frame(lane, arena, lane->active);
   if (!frame || lane->error)
      return false;
   lane->context.action = KK_RT_RETURN;
   lane->context.resume = 0;
   lane->context.frame_size = 0;
   frame->saved = lane->context;
   return true;
}

static inline bool
kk_rt_stack_suspend(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                     KK_RT_DEVICE unsigned char *arena, KK_RT_U32 resume_pc)
{
   KK_RT_DEVICE struct kk_rt_cps_frame *frame =
      kk_rt_stack_frame(lane, arena, lane->active);
   if (!frame || lane->error)
      return false;
   if (resume_pc == KK_RT_STOP || !lane->context.resume ||
       lane->context.frame_size > frame->end - frame->scratch)
      return kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
   frame->resume_pc = resume_pc;
   return true;
}

/* Switch to a retained activation without freeing younger allocations. This
 * is the REPORT/any-hit seam; restores are explicit so acceptance can be
 * published into the procedural context before its resume function runs. */
static inline bool
kk_rt_stack_activate(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                      KK_RT_DEVICE unsigned char *arena, KK_RT_U64 target,
                      KK_RT_U32 pc)
{
   if (lane->error || pc == KK_RT_STOP)
      return kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
   KK_RT_U64 current = lane->top;
   while (current != KK_RT_NO_FRAME) {
      KK_RT_DEVICE struct kk_rt_cps_frame *frame =
         kk_rt_stack_frame(lane, arena, current);
      if (!frame)
         return false;
      if (current == target) {
         lane->active = target;
         lane->context = frame->saved;
         lane->pc = pc;
         return true;
      }
      if (frame->previous != KK_RT_NO_FRAME && frame->previous >= current)
         break;
      current = frame->previous;
   }
   return kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
}

static inline bool
kk_rt_stack_return(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                    KK_RT_DEVICE unsigned char *arena)
{
   if (lane->error || lane->active != lane->top)
      return kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
   KK_RT_DEVICE struct kk_rt_cps_frame *frame =
      kk_rt_stack_frame(lane, arena, lane->active);
   if (!frame)
      return false;
   KK_RT_U64 parent = frame->parent;
   lane->top = frame->previous;
   lane->end = frame->previous_end;
   if (parent == KK_RT_NO_FRAME) {
      if (lane->top != KK_RT_NO_FRAME)
         return kk_rt_stack_fail(lane, KK_RT_STACK_STATE);
      lane->active = KK_RT_NO_FRAME;
      lane->pc = KK_RT_STOP;
      return true;
   }
   KK_RT_DEVICE struct kk_rt_cps_frame *caller =
      kk_rt_stack_frame(lane, arena, parent);
   return caller && kk_rt_stack_activate(lane, arena, parent, caller->resume_pc);
}

/* Termination may abandon a parked procedural activation and its children.
 * Only allocations strictly younger than the retained target are released. */
static inline bool
kk_rt_stack_unwind(KK_RT_DEVICE struct kk_rt_lane_state *lane,
                    KK_RT_DEVICE unsigned char *arena, KK_RT_U64 target,
                    KK_RT_U32 pc)
{
   if (!kk_rt_stack_activate(lane, arena, target, pc))
      return false;
   while (lane->top != target) {
      KK_RT_DEVICE struct kk_rt_cps_frame *frame =
         kk_rt_stack_frame(lane, arena, lane->top);
      if (!frame)
         return false;
      lane->top = frame->previous;
      lane->end = frame->previous_end;
   }
   return true;
}

#undef KK_RT_DEVICE
#undef KK_RT_THREAD
#undef KK_RT_U64
#undef KK_RT_U32
#undef KK_RT_ASSERT
#endif
