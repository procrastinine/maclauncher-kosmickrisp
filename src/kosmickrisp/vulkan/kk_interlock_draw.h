/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_DRAW_H
#define KK_INTERLOCK_DRAW_H

#include "vulkan/vulkan_core.h"
#include "kosmickrisp/bridge/mtl_types.h"

struct kk_cmd_buffer;
struct kk_device;

/* The same constructor serves command recording and private continuations.
 * Its fixed slots and capacity remain owned by the shared argument-table API. */
mtl_argument_table *kk_new_command_argument_table(struct kk_device *dev);
VkResult kk_interlock_record_draw(struct kk_cmd_buffer *cmd);

#endif
