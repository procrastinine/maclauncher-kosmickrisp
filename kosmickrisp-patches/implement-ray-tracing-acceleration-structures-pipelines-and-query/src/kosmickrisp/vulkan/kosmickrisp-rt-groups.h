/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_GROUPS_H
#define KK_RT_GROUPS_H
#include "vk_pipeline.h"
#include "kk_rt_sbt.h"
VkResult kk_rt_pipeline_groups(struct vk_device *device, const struct vk_pipeline *common,
                                const VkAllocationCallbacks *alloc,
                                struct kk_rt_group_entry **groups_out);
void kk_rt_write_group(struct vk_device *device, VkRayTracingShaderGroupTypeKHR type,
                        const struct vk_shader **shaders, uint32_t count, void *output);
#endif
