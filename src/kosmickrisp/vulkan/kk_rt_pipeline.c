/* SPDX-License-Identifier: MIT */
#include "kosmickrisp-rt-pipeline.h"
#include "kosmickrisp-rt-groups.h"
#include "kosmickrisp-rt-commands.h"
#include "kosmickrisp-rt-shader.h"
#include "kosmickrisp-rt-dispatch.h"
#include "kosmickrisp-rt-cps.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_physical_device.h"
#include "vk_alloc.h"
#include "vk_pipeline.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "kosmickrisp/bridge/mtl_residency_set.h"
#include "util/ralloc.h"
#include "compiler/spirv/spirv.h"

void kk_compile_msl(struct kk_device *, nir_shader *, const struct vk_graphics_pipeline_state *,struct msl_compile_data *);

static void
kk_rt_embedded_finish(struct kk_rt_pipeline *pipeline)
{
   if (pipeline->function_table_resident) {
      struct kk_device *dev = container_of(pipeline->dispatch_shader.vk.base.device, struct kk_device, vk);
      simple_mtx_lock(&dev->residency_set.mutex);
      mtl_residency_set_remove_allocation(dev->residency_set.handle, pipeline->native.function_table);
      simple_mtx_unlock(&dev->residency_set.mutex);
      pipeline->function_table_resident = false;
   }
   /* Only native owns these handles; the typed command wrapper aliases the
    * compute PSO but is never sent to ordinary freeing shader destruction. */
   pipeline->dispatch_shader.pipeline.cs = NULL;
   kk_rt_native_pipeline_finish(&pipeline->native);
   mtl_release(pipeline->control);
   pipeline->control=NULL;
   mtl_release(pipeline->dispatcher);
   mtl_release(pipeline->dispatcher_library);
   pipeline->dispatcher = NULL;
   pipeline->dispatcher_library = NULL;
   if (pipeline->dispatch_initialized) {
      vk_object_base_finish(&pipeline->dispatch_shader.vk.base);
      pipeline->dispatch_initialized = false;
   }
}

static void
kk_rt_embedded_destroy(struct vk_device *device, struct vk_shader *shader,
                        const VkAllocationCallbacks *alloc)
{
   struct kk_shader *dispatch = container_of(shader, struct kk_shader, vk);
   struct kk_rt_pipeline *pipeline = container_of(dispatch, struct kk_rt_pipeline, dispatch_shader);
   /* No vk_shader_free, cache unref or outer free belongs to this member. */
   kk_rt_embedded_finish(pipeline);
}

static const struct vk_shader_ops kk_rt_embedded_ops = {
   .destroy = kk_rt_embedded_destroy,
};

static struct kk_rt_pipeline *
kk_rt_pipeline_private_create(struct vk_device *device,
                               const VkAllocationCallbacks *alloc)
{
   struct kk_rt_pipeline *pipeline = vk_zalloc2(&device->alloc, alloc, sizeof(*pipeline),
      8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return NULL;
   struct kk_shader *dispatch = &pipeline->dispatch_shader;
   vk_object_base_init(device, &dispatch->vk.base, VK_OBJECT_TYPE_SHADER_EXT);
   dispatch->vk.ops = &kk_rt_embedded_ops;
   dispatch->vk.stage = MESA_SHADER_RAYGEN;
   dispatch->info.stage = MESA_SHADER_RAYGEN;
   pipeline->dispatch_initialized = true;
   return pipeline;
}

void
kk_rt_pipeline_destroy(struct vk_device *device, void *data,
                        const VkAllocationCallbacks *alloc)
{
   struct kk_rt_pipeline *pipeline = data;
   if (!pipeline)
      return;
   kk_rt_embedded_finish(pipeline);
   if(pipeline->traversal) pipeline->traversal->ops->destroy(device,pipeline->traversal,alloc);
   vk_free2(&device->alloc, alloc, pipeline->groups);
   vk_free2(&device->alloc, alloc, pipeline->programs);
   vk_free2(&device->alloc, alloc, pipeline->functions);
   vk_free2(&device->alloc, alloc, pipeline);
}

VkResult
kk_rt_pipeline_realize(struct vk_device *device, const struct vk_pipeline *common,
                        const VkAllocationCallbacks *alloc, void **data)
{
   assert(*data == NULL);
   /* Libraries keep the actual common compiled stages and stable identities.
    * The executable that imports them owns the final native PSO and VFT. */
   struct kk_rt_group_entry *groups=NULL;
   VkResult result=kk_rt_pipeline_groups(device,common,alloc,&groups);
   if(result!=VK_SUCCESS)return result;
   if (common->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) {
      vk_free2(&device->alloc,alloc,groups);
      return VK_SUCCESS;
   }
   /* Shader cache hits do not retain a native executable PSO. Respect the
    * caller's compile prohibition before compiling or linking that PSO. */
   if(common->flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR){
      vk_free2(&device->alloc,alloc,groups);
      return VK_PIPELINE_COMPILE_REQUIRED;
   }
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   struct kk_rt_pipeline *pipeline = kk_rt_pipeline_private_create(device, alloc);
   if (!pipeline) { vk_free2(&device->alloc,alloc,groups); return VK_ERROR_OUT_OF_HOST_MEMORY; }
   pipeline->groups=groups; pipeline->group_count=vk_pipeline_get_rt_group_count(common);
   *data = pipeline; /* Common owns partial state on every later failure. */
   pipeline->traversal_program=vk_pipeline_get_rt_stage_count(common);
   if(pipeline->traversal_program==UINT32_MAX)return VK_ERROR_OUT_OF_HOST_MEMORY;
   pipeline->program_count = pipeline->traversal_program+1;
   uint32_t pipeline_ray_flags =
      ((common->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR) ? SpvRayFlagsSkipTrianglesKHRMask : 0) |
      ((common->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_SKIP_AABBS_BIT_KHR) ? SpvRayFlagsSkipAABBsKHRMask : 0) |
      ((common->flags & VK_PIPELINE_CREATE_2_RAY_TRACING_OPACITY_MICROMAP_BIT_KHR) ? KK_RT_PIPELINE_OPACITY_MICROMAP : 0);
   result=kk_rt_compile_traversal(device,alloc,&pipeline->traversal,kk_compile_msl,pipeline_ray_flags);
   if(result!=VK_SUCCESS)return result;
   uint64_t bytes;
   if (!pipeline->program_count ||
       !kk_rt_u64_mul(pipeline->program_count, sizeof(*pipeline->programs), &bytes) || bytes > SIZE_MAX)
      return VK_ERROR_INVALID_SHADER_NV;
   pipeline->programs = vk_zalloc2(&device->alloc, alloc, bytes, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline->programs)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   uint64_t functions = 0;
   for (uint32_t i = 0; i < pipeline->program_count; ++i) {
      const struct vk_shader *base = i==pipeline->traversal_program?pipeline->traversal:vk_pipeline_get_rt_stage(common, i);
      const struct kk_rt_shader *shader = kk_rt_shader_from_vk(base);
      if (!shader || !shader->function_count ||
          !kk_rt_u64_add(functions, shader->function_count, &functions) || functions >= UINT32_MAX)
         return VK_ERROR_INVALID_SHADER_NV;
      pipeline->attribute_size=MAX2(pipeline->attribute_size,MAX2(shader->attribute_size,32));
      pipeline->dispatch_shader.info.push_size=MAX2(pipeline->dispatch_shader.info.push_size,shader->push_size);
      pipeline->dispatch_shader.info.descriptor_heap|=shader->descriptor_heap;
      pipeline->stack_stride=MAX2(pipeline->stack_stride,base->stack_size);
      pipeline->programs[i] = (struct kk_rt_program_binding){base,
         functions - shader->function_count, shader->function_count};
   }
   if(!kk_rt_u64_add(functions,1,&functions)||functions>UINT32_MAX)return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!kk_rt_u64_mul(functions, sizeof(mtl_function_descriptor *), &bytes) || bytes > SIZE_MAX)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   mtl_function_descriptor **descriptors = vk_alloc2(&device->alloc, alloc, bytes,
      8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!descriptors)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   uint64_t info_bytes;
   if (!kk_rt_u64_mul(functions, sizeof(*pipeline->functions), &info_bytes) || info_bytes > SIZE_MAX) {
      vk_free2(&device->alloc, alloc, descriptors);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pipeline->functions = vk_alloc2(&device->alloc, alloc, info_bytes, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline->functions) {
      vk_free2(&device->alloc, alloc, descriptors);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   uint32_t count = 0;
   for (uint32_t i = 0; i < pipeline->program_count; ++i) {
      const struct kk_rt_shader *shader = kk_rt_shader_from_vk(pipeline->programs[i].shader);
      for (uint32_t j = 0; j < shader->function_count; ++j) {
         const struct kk_rt_function *f = &shader->functions[j];
         for (uint32_t p = 0; p <= i; ++p) {
            const struct kk_rt_shader *prior = kk_rt_shader_from_vk(pipeline->programs[p].shader);
            uint32_t limit = p == i ? j : prior->function_count;
            for (uint32_t q = 0; q < limit; ++q) {
               const struct kk_rt_function *other = &prior->functions[q];
               if (strcmp(f->name, other->name) == 0 &&
                   (strcmp(f->source, other->source) != 0 || f->requires_msl41 != other->requires_msl41 ||
                    f->uses_atomic64_locks != other->uses_atomic64_locks)) {
                  vk_free2(&device->alloc, alloc, descriptors);
                  return VK_ERROR_INVALID_SHADER_NV;
               }
            }
         }
         pipeline->functions[count] = (struct kk_rt_function_info){KK_RT_FUNCTION_APPLICATION, i};
         descriptors[count++] = shader->functions[j].descriptor;
      }
   }
   pipeline->function_count = count+1;
   pipeline->functions[count]=(struct kk_rt_function_info){KK_RT_FUNCTION_CONTROL,KK_RT_STOP};
   char *source = kk_rt_dispatcher_source(NULL);
   if (!source) {
      vk_free2(&device->alloc, alloc, descriptors);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pipeline->dispatcher_library = mtl_new_library(dev->mtl_compiler_handle, source,
      MTL_LANGUAGE_VERSION_4_0, MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   ralloc_free(source);
   if (pipeline->dispatcher_library)
      pipeline->dispatcher = mtl_new_library_function_descriptor(pipeline->dispatcher_library, "main_entrypoint");
   if(pipeline->dispatcher_library) pipeline->control=mtl_new_library_function_descriptor(pipeline->dispatcher_library,"kk_rt_control");
   descriptors[count++]=pipeline->control;
   bool linked = pipeline->dispatcher && pipeline->control && kk_rt_link_functions(dev->mtl_compiler_handle,
      pipeline->dispatcher, count, descriptors, 32, &pipeline->native);
   vk_free2(&device->alloc, alloc, descriptors);
   if (!linked)
      return VK_ERROR_INVALID_SHADER_NV;
   simple_mtx_lock(&dev->residency_set.mutex);
   mtl_residency_set_add_allocation(dev->residency_set.handle, pipeline->native.function_table);
   simple_mtx_unlock(&dev->residency_set.mutex);
   pipeline->function_table_resident = true;
   pipeline->recursion_depth = vk_pipeline_get_rt_recursion_depth(common);
   /* stack_size already includes the CPS frame header. Reserve the actual
    * pipeline recursion depth, plus callable/parked activations, rather than
    * charging every launch for the device maximum of 31 trace levels. */
   uint64_t per_frame, frame_count;
   if (!kk_rt_u64_add(pipeline->stack_stride, 2 * pipeline->attribute_size, &per_frame) ||
       !kk_rt_u64_add(per_frame, 15, &per_frame) ||
       !kk_rt_u64_mul(pipeline->recursion_depth, 3, &frame_count) ||
       !kk_rt_u64_add(frame_count, 3, &frame_count) ||
       !kk_rt_u64_mul(per_frame & ~15ull, frame_count, &pipeline->stack_stride))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   uint64_t common_stack = vk_pipeline_get_rt_stack_size(common), traversal_stack;
   if (!kk_rt_u64_mul(pipeline->traversal->stack_size + 2 * pipeline->attribute_size,
                      pipeline->recursion_depth, &traversal_stack) ||
       !kk_rt_u64_add(common_stack, traversal_stack, &common_stack))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   pipeline->stack_stride = MAX2(pipeline->stack_stride, common_stack);
   pipeline->dispatch_shader.pipeline.cs = pipeline->native.pipeline;
   return VK_SUCCESS;
}

void
kk_rt_pipeline_bind(struct vk_command_buffer *base, const struct vk_pipeline *common, void *data)
{
   struct kk_cmd_buffer *cmd = container_of(base, struct kk_cmd_buffer, vk);
   struct kk_rt_pipeline *pipeline = data;
   if(pipeline){
      struct kk_descriptor_state *desc=kk_rt_descriptors(cmd);
      if(!desc)return;
      desc->root_dirty|=desc->descriptor_heap!=pipeline->dispatch_shader.info.descriptor_heap;
      desc->descriptor_heap=pipeline->dispatch_shader.info.descriptor_heap;
      desc->root_dirty |= desc->push_size != pipeline->dispatch_shader.info.push_size;
      desc->push_size = pipeline->dispatch_shader.info.push_size;
   }
   /* This callback alone writes the RT identity slot. Neither descriptor
    * bindpoint nor compute/graphics shader state is substituted for RT. */
   cmd->state.shaders[MESA_SHADER_RAYGEN] = pipeline ? &pipeline->dispatch_shader : NULL;
}

const struct kk_rt_pipeline *
kk_rt_bound_pipeline(const struct kk_cmd_buffer *cmd)
{
   if (!(cmd->vk.pipeline_shader_stages & VK_SHADER_STAGE_RAYGEN_BIT_KHR))
      return NULL;
   const struct kk_shader *dispatch = cmd->state.shaders[MESA_SHADER_RAYGEN];
   if (!dispatch || dispatch->vk.ops != &kk_rt_embedded_ops)
      return NULL;
   return container_of(dispatch, const struct kk_rt_pipeline, dispatch_shader);
}
