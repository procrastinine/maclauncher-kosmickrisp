/* SPDX-License-Identifier: MIT */
#ifndef KK_ASYNC_COMPUTE_H
#define KK_ASYNC_COMPUTE_H
#include <stdbool.h>
#include "vulkan/vulkan_core.h"
struct kk_device;
struct kk_shader;
struct kk_async_compute;
struct kk_async_compute_owner;
extern _Thread_local bool kk_defer_compute;
bool kk_async_compute_can_defer(uint32_t count, const VkComputePipelineCreateInfo *infos);
bool kk_async_compute_start(struct kk_device *dev, struct kk_shader *shader,
                           VkResult (*compile)(struct kk_device *, struct kk_shader *));
VkResult kk_async_compute_join(struct kk_shader *shader);
void kk_async_compute_dispatch(struct kk_shader *shader);
bool kk_async_compute_serializable(struct kk_shader *shader);
void kk_async_compute_shader_finish(struct kk_shader *shader);
void kk_async_compute_drain(struct kk_device *dev);
void kk_async_compute_device_finish(struct kk_device *dev);
VkResult kk_async_compute_finish_creation(struct kk_device *dev, uint32_t count,
   const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *allocator,
   VkPipeline *pipelines, VkResult result, bool force_wait);
#endif
