/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_bridge.h"
#include "vk_cmd_queue.h"
#import <Foundation/NSAutoreleasePool.h>

// kk_image_layout.h should also includes "vulkan/vulkan.h", but just to be safe
#include "vulkan/vulkan.h"
#include "kk_image_layout.h"

#include "util/macros.h"

#include <Metal/MTLCommandBuffer.h>
#include <Metal/MTLCommandQueue.h>
#include <Metal/MTLDevice.h>
#include <Metal/MTLHeap.h>
#include <Metal/MTLEvent.h>

#include <QuartzCore/CAMetalLayer.h>

static_assert(sizeof(MTLResourceID) == sizeof(uint64_t), "Must match, otherwise descriptors are broken");

mtl_texture *
mtl_drawable_get_texture(void *drawable_ptr)
{
   @autoreleasepool {
      id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)drawable_ptr;
      return drawable.texture;
   }
}

void *
mtl_retain(void *handle)
{
   @autoreleasepool {
      NSObject *obj = (NSObject *)handle;
      return [obj retain];
   }
}

void
mtl_release(void *handle)
{
   @autoreleasepool {
      NSObject *obj = (NSObject *)handle;
      [obj release];
   }
}

/* Thread-local dynamic extent, never shared between recording workers. */
static _Thread_local bool kk_mtl_replay_pool_active;

bool
mtl_replay_autorelease_pool_active(void)
{
   return kk_mtl_replay_pool_active;
}

void
mtl_execute_command_queue(struct vk_cmd_queue *queue,
                          VkCommandBuffer command_buffer,
                          const struct vk_device_dispatch_table *dispatch)
{
   NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
   if (pool == nil) {
      /* An existing outer scope or the standalone wrappers still owns pools. */
      vk_cmd_queue_execute(queue, command_buffer, dispatch);
      return;
   }
   const bool previous = kk_mtl_replay_pool_active;
   kk_mtl_replay_pool_active = true;
   @try {
      vk_cmd_queue_execute(queue, command_buffer, dispatch);
   } @finally {
      /* Restore before draining: deallocation may reenter the bridge. */
      kk_mtl_replay_pool_active = previous;
      [pool drain];
   }
}
