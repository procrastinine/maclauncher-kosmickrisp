/* SPDX-License-Identifier: MIT */
#ifndef KK_MTL_REPLAY_AUTORELEASE_H
#define KK_MTL_REPLAY_AUTORELEASE_H
#include <stdbool.h>
#include <vulkan/vulkan_core.h>
struct vk_cmd_queue;
struct vk_device_dispatch_table;
/* This marker is true only inside the synchronous lexical replay pool. */
bool mtl_replay_autorelease_pool_active(void);
void mtl_execute_command_queue(struct vk_cmd_queue *queue,
                               VkCommandBuffer command_buffer,
                               const struct vk_device_dispatch_table *dispatch);
#ifdef __OBJC__
/* Standalone bridge calls retain their original lexical pools,
 * including cleanup on ordinary early returns. */
#define KK_MTL_REPLAY_POOL(...)                                      \
   do {                                                             \
      if (mtl_replay_autorelease_pool_active()) { __VA_ARGS__ }       \
      else { @autoreleasepool { __VA_ARGS__ } }                      \
   } while (0)
#endif
#endif
