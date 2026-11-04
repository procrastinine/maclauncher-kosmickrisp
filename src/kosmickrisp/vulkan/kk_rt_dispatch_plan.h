/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_DISPATCH_PLAN_H
#define KK_RT_DISPATCH_PLAN_H
#include "kk_rt_stack.h"

/* One SIMD scheduler per 32-thread group. Keep enough independent groups to
 * occupy the GPU; bound only their private arena, never the logical launch.
 * A larger direct launch runs as consecutive dispatches over the same arena,
 * at most KK_RT_MAX_LAUNCH_DISPATCHES of them. Indirect dimensions are read
 * on the GPU and grid-stride the arena in one dispatch. */
#define KK_RT_MAX_LAUNCH_DISPATCHES 1024u
static inline bool
kk_rt_dispatch_plan(uint64_t launches, bool indirect, uint64_t stride,
                    uint64_t arena_budget, uint64_t max_buffer,
                    uint32_t *physical, uint64_t *stack_bytes, uint64_t *lane_bytes)
{
   uint64_t per_lane, capacity, desired;
   if (!stride || (stride & 15) ||
       !kk_rt_u64_add(stride, sizeof(struct kk_rt_lane_state), &per_lane))
      return false;
   capacity = arena_budget / per_lane;
   uint64_t stack_capacity = max_buffer / stride;
   uint64_t lane_capacity = max_buffer / sizeof(struct kk_rt_lane_state);
   if (capacity > stack_capacity) capacity = stack_capacity;
   if (capacity > lane_capacity) capacity = lane_capacity;
   if (capacity > UINT32_MAX - 31u) capacity = UINT32_MAX - 31u;
   capacity &= ~31ull;
   if (!capacity) return false;
   if (indirect) desired = capacity;
   else {
      if (!launches || !kk_rt_u64_add(launches, 31, &desired)) return false;
      desired &= ~31ull;
      if (desired > capacity) desired = capacity;
   }
   if (!kk_rt_u64_mul(stride, desired, stack_bytes) ||
       !kk_rt_u64_mul(sizeof(struct kk_rt_lane_state), desired, lane_bytes))
      return false;
   *physical = desired;
   return true;
}
#endif
