/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_PIPELINE_H
#define KK_RT_PIPELINE_H
#include "kk_shader.h"
#include "kosmickrisp-rt-metal.h"
#include "kosmickrisp-rt-dispatch.h"
#include "kk_rt_sbt.h"

struct kk_cmd_buffer;
struct kk_rt_program_binding {
   const struct vk_shader *shader;
   uint32_t function_first;
   uint32_t function_count;
};
struct kk_rt_pipeline {
   /* Typed, borrowed command binding. It is neither a public VkShaderEXT nor
    * an independently allocated/cached/refcounted common stage object. */
   struct kk_shader dispatch_shader;
   bool dispatch_initialized;
   bool function_table_resident;
   struct kk_rt_native_pipeline native;
   mtl_library *dispatcher_library;
   mtl_function_descriptor *dispatcher;
   mtl_function_descriptor *control;
   struct vk_shader *traversal;
   uint32_t traversal_program;
   uint64_t stack_stride;
   uint64_t attribute_size;
   struct kk_rt_group_entry *groups;
   uint32_t group_count;
   uint32_t program_count;
   uint32_t function_count;
   struct kk_rt_program_binding *programs;
   struct kk_rt_function_info *functions;
};
VkResult kk_rt_pipeline_realize(struct vk_device *device,
                                 const struct vk_pipeline *pipeline,
                                 const VkAllocationCallbacks *alloc, void **data);
void kk_rt_pipeline_destroy(struct vk_device *device, void *data,
                             const VkAllocationCallbacks *alloc);
void kk_rt_pipeline_bind(struct vk_command_buffer *cmd,
                          const struct vk_pipeline *pipeline, void *data);
const struct kk_rt_pipeline *kk_rt_bound_pipeline(const struct kk_cmd_buffer *cmd);
#endif
