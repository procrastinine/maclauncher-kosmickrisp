#include "mtl_gpu_timing.h"
/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_command_buffer.h"

#include <Metal/MTL4BufferRange.h>
#include <Metal/MTL4CommandAllocator.h>
#include <Metal/MTL4CommandBuffer.h>
#include <objc/runtime.h>

static char kk_command_buffer_allocator_key;
#include <Metal/MTL4Counters.h>

void
mtl_command_allocator_reset(mtl_command_allocator *allocator)
{
   @autoreleasepool {
      id<MTL4CommandAllocator> alloc = (id<MTL4CommandAllocator>)allocator;
      [alloc reset];
   }
}

void
mtl_begin_command_buffer(mtl_command_buffer *command_buffer,
                         mtl_command_allocator *allocator)
{
   @autoreleasepool {
      id<MTL4CommandBuffer> cmd_buf = (id<MTL4CommandBuffer>)command_buffer;
      id<MTL4CommandAllocator> alloc = (id<MTL4CommandAllocator>)allocator;
      /* The previous allocator must survive the native begin call that
       * returns its old-generation storage. Keep the new allocator alive
       * through native buffer deallocation, including after pool reset. */
      [cmd_buf beginCommandBufferWithAllocator:alloc];
      if (__atomic_load_n(&mtl_gpu_timing_enabled, __ATOMIC_ACQUIRE))
         mtl_gpu_timing_reset_command(command_buffer);
      objc_setAssociatedObject(cmd_buf, &kk_command_buffer_allocator_key,
                               alloc, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   }
}

void
mtl_end_command_buffer(mtl_command_buffer *command_buffer)
{
   @autoreleasepool {
      id<MTL4CommandBuffer> cmd_buf = (id<MTL4CommandBuffer>)command_buffer;
      [cmd_buf endCommandBuffer];
   }
}

void
mtl_command_resolve_counter_heap(mtl_command_buffer *command_buffer,
                                 mtl_counter_heap *heap, uint32_t first_index,
                                 uint32_t count, uint64_t dst_addr)
{
   @autoreleasepool {
      id<MTL4CommandBuffer> cmd_buf = (id<MTL4CommandBuffer>)command_buffer;
      id<MTL4CounterHeap> h = (id<MTL4CounterHeap>)heap;
      MTL4BufferRange range = {
         .bufferAddress = dst_addr,
         .length = count * sizeof(MTL4TimestampHeapEntry),
      };
      [cmd_buf resolveCounterHeap:h
                        withRange:NSMakeRange(first_index, count)
                       intoBuffer:range
                        waitFence:nil
                      updateFence:nil];
   }
}

void
mtl_command_buffer_set_label(mtl_command_buffer *command_buffer,
                             const char *label)
{
   @autoreleasepool {
      id<MTL4CommandBuffer> cb = (id<MTL4CommandBuffer>)command_buffer;
      cb.label = @(label);
   }
}

void
mtl_command_buffer_push_debug_group(mtl_command_buffer *command_buffer,
                                    const char *label)
{
    @autoreleasepool {
      id<MTL4CommandBuffer> cb = (id<MTL4CommandBuffer>)command_buffer;
      [cb pushDebugGroup:@(label)];
    }
}

void mtl_command_buffer_pop_debug_group(mtl_command_buffer *command_buffer)
{
   @autoreleasepool {
      id<MTL4CommandBuffer> cb = (id<MTL4CommandBuffer>)command_buffer;
      [cb popDebugGroup];
   }
}
