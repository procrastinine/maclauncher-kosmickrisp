/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_COMMANDS_H
#define KK_RT_COMMANDS_H
#include "kk_cmd_buffer.h"
#define KK_RT_STAGES (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)
struct kk_rt_command_state {
   struct kk_descriptor_state descriptors;
   uint64_t scratch_size;
   uint64_t stack_size;
   uint32_t ray_queries;
};
struct kk_descriptor_state *kk_rt_descriptors(struct kk_cmd_buffer *cmd);
void kk_rt_descriptor_dispatch_init(struct kk_device *dev);
void kk_rt_heap_dispatch_init(struct kk_device *dev);
void kk_rt_command_finish(struct kk_cmd_buffer *cmd);
void kk_rt_set_state(struct vk_command_buffer *cmd, struct vk_pipeline_layout *layout,
                      VkDeviceSize scratch_size, uint32_t queries, const uint8_t *dynamic_offsets);
void kk_rt_set_stack_size(struct vk_command_buffer *cmd, VkDeviceSize size);
#endif
