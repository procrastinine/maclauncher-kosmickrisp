/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_bridge.h"
#include "vk_cmd_queue.h"

mtl_texture *
mtl_drawable_get_texture(void *drawable_ptr)
{
   return NULL;
}

void *
mtl_retain(void *handle)
{
   return NULL;
}

void
mtl_release(void *handle)
{
}

bool
mtl_replay_autorelease_pool_active(void)
{
   return false;
}

void
mtl_execute_command_queue(struct vk_cmd_queue *queue,
                          VkCommandBuffer command_buffer,
                          const struct vk_device_dispatch_table *dispatch)
{
   vk_cmd_queue_execute(queue, command_buffer, dispatch);
}
