/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_SHADER_H
#define KK_RT_SHADER_H
#include "vk_shader.h"
#include "nir/nir.h"
#include "kosmickrisp/bridge/mtl_types.h"

struct kk_device;
struct msl_compile_data;
struct blob_reader;
struct kk_rt_function {
   char *name;
   char *source;
   bool requires_msl41;
   bool uses_atomic64_locks;
   bool uses_sampler_emulation;
   bool requires_sampler_emulation;
   mtl_library *library;
   mtl_function_descriptor *descriptor;
};
struct kk_rt_shader {
   struct vk_shader vk;
   uint64_t payload_size;
   uint64_t attribute_size;
   uint32_t push_size;
   bool descriptor_heap;
   uint32_t embedded_sampler_count;
   struct vk_sampler_state *embedded_sampler_states;
   uint64_t embedded_sampler_table;
   uint32_t function_count;
   struct kk_rt_function *functions;
};
void kk_compile_msl(struct kk_device *dev,nir_shader *nir,const struct vk_graphics_pipeline_state *state,struct msl_compile_data *data);
typedef void (*kk_rt_compile_part)(struct kk_device *dev,
                                  nir_shader *nir,
                                  const struct vk_graphics_pipeline_state *state,
                                  struct msl_compile_data *data);

VkResult kk_rt_compile_lowered(struct vk_device *device, uint32_t count,
                                nir_shader **shaders, const struct vk_shader_compile_info *infos, uint32_t heap_push_size,
                                const VkAllocationCallbacks *alloc,
                                struct vk_shader **out,
                                kk_rt_compile_part compile_part);
VkResult kk_rt_compile_traversal(struct vk_device *device, const VkAllocationCallbacks *alloc, struct vk_shader **out, kk_rt_compile_part compile_part, uint32_t pipeline_ray_flags);
bool kk_rt_shader_binary(const struct blob_reader *blob);
VkResult kk_rt_shader_deserialize(struct vk_device *device, struct blob_reader *blob,
                                   const VkAllocationCallbacks *alloc,
                                   struct vk_shader **out);
const struct kk_rt_shader *kk_rt_shader_from_vk(const struct vk_shader *shader);
#endif
