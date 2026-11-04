/* SPDX-License-Identifier: MIT */
#include "kosmickrisp-rt-commands.h"
#include "kosmickrisp-rt-pipeline.h"
#include "kosmickrisp-rt-shader.h"

#include "kk_device.h"
#include "kk_cmd_pool.h"
#include "vk_alloc.h"
#include "vk_pipeline_layout.h"

struct kk_descriptor_state *
kk_rt_descriptors(struct kk_cmd_buffer *cmd)
{
   if (!cmd->state.rt) {
      cmd->state.rt = vk_zalloc(&kk_cmd_buffer_pool(cmd)->vk.alloc,
         sizeof(*cmd->state.rt), 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!cmd->state.rt) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return NULL;
      }
      cmd->state.rt->descriptors.root_dirty = true;
      /* Heap bindings are global; seed a newly materialized RT store from the
       * existing latest binding, then retain it independently. */
      cmd->state.rt->descriptors.heap_sampler = cmd->state.gfx.descriptors.heap_sampler;
      cmd->state.rt->descriptors.heap_resource = cmd->state.gfx.descriptors.heap_resource;
   }
   return &cmd->state.rt->descriptors;
}
void
kk_rt_command_finish(struct kk_cmd_buffer *cmd)
{
   vk_free(&kk_cmd_buffer_pool(cmd)->vk.alloc, cmd->state.rt);
   cmd->state.rt = NULL;
}
void
kk_rt_set_state(struct vk_command_buffer *base, struct vk_pipeline_layout *layout,
                  VkDeviceSize scratch_size, uint32_t queries, const uint8_t *offsets)
{
   struct kk_cmd_buffer *cmd = container_of(base, struct kk_cmd_buffer, vk);
   struct kk_descriptor_state *desc = kk_rt_descriptors(cmd);
   if (!desc) return;
   cmd->state.rt->scratch_size = scratch_size;
   cmd->state.rt->ray_queries = queries;
   memcpy(desc->root.set_dynamic_buffer_start, offsets, sizeof(desc->root.set_dynamic_buffer_start));
   uint32_t push_size = 0;
   if (layout) for (uint32_t i=0;i<layout->push_range_count;++i)
      if (layout->push_ranges[i].stageFlags & KK_RT_STAGES)
         push_size=MAX2(push_size,layout->push_ranges[i].offset+layout->push_ranges[i].size);
   desc->root_dirty |= push_size != desc->push_size;
   desc->push_size = push_size;
}
void
kk_rt_set_stack_size(struct vk_command_buffer *base, VkDeviceSize size)
{
   struct kk_cmd_buffer *cmd = container_of(base, struct kk_cmd_buffer, vk);
   if (kk_rt_descriptors(cmd)) cmd->state.rt->stack_size = size;
}

#include "kk_entrypoints.h"
#include "kk_bo.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_sampler.h"
#include "vk_util.h"
#include "kk_physical_device.h"
#include "kk_rt_dispatch_plan.h"
#include "kosmickrisp/bridge/mtl_device.h"
static struct kk_ptr
kk_rt_arena(struct kk_cmd_buffer *cmd,uint64_t bytes)
{
   if(bytes<=UINT32_MAX)return kk_pool_alloc(cmd,bytes,16);
   struct kk_device *dev=kk_cmd_buffer_device(cmd);struct kk_bo *bo=NULL;
   VkResult result=kk_alloc_bo(dev,&cmd->vk.base,bytes,16,&bo);
   if(result!=VK_SUCCESS){vk_command_buffer_set_error(&cmd->vk,result);return(struct kk_ptr){0};}
   util_dynarray_append(&cmd->large_bos,bo);return(struct kk_ptr){.cpu=bo->cpu,.gpu=bo->gpu,.buffer=bo->map};
}
static void
kk_rt_trace(struct kk_cmd_buffer *cmd,const VkStridedDeviceAddressRegionKHR *raygen,
              const VkStridedDeviceAddressRegionKHR *miss,const VkStridedDeviceAddressRegionKHR *hit,
              const VkStridedDeviceAddressRegionKHR *callable,uint32_t width,uint32_t height,uint32_t depth,
              uint64_t indirect,uint64_t indirect2)
{
   const struct kk_rt_pipeline *pipeline=kk_rt_bound_pipeline(cmd);
   if(!pipeline){vk_command_buffer_set_error(&cmd->vk,VK_ERROR_INVALID_SHADER_NV);return;}
   struct kk_descriptor_state *desc=kk_rt_descriptors(cmd);if(!desc)return;
   if(!indirect&&!indirect2&&(!width||!height||!depth))return;
   if(desc->push_dirty)kk_cmd_buffer_flush_push_descriptors(cmd,desc);
   if(desc->root_dirty)kk_upload_descriptor_root(cmd,VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   if(!desc->root.addr)return;
   uint64_t bytes,stride=MAX2(pipeline->stack_stride,cmd->state.rt->stack_size);
   if(!kk_rt_u64_add(stride,15,&stride)){vk_command_buffer_set_error(&cmd->vk,VK_ERROR_OUT_OF_DEVICE_MEMORY);return;}
   stride&=~15ull;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   const struct kk_physical_device *pdev = kk_device_physical(dev);
   uint64_t launches = 0, lane_bytes;
   uint32_t physical;
   bool is_indirect = indirect || indirect2;
   /* Reuse each lane arena across its launches when a full grid would
    * consume more than one eighth of the device working set. */
   uint64_t budget = mtl_device_recommended_max_working_set_size(dev->mtl_handle) / 8;
   if ((!is_indirect && (!kk_rt_u64_mul(width, height, &launches) ||
                         !kk_rt_u64_mul(launches, depth, &launches))) ||
       !kk_rt_dispatch_plan(launches, is_indirect, stride, budget,
                            pdev->info.max_buffer_size, &physical, &bytes, &lane_bytes)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }
   struct kk_ptr stack = kk_rt_arena(cmd, bytes), lanes = kk_rt_arena(cmd, lane_bytes);
   if(!stack.gpu||!lanes.gpu)return;
   struct kk_rt_dispatch_root root={0};root.functions=pipeline->native.function_table_id;
   root.root=desc->root.addr;root.sampler_table=kk_cmd_buffer_device(cmd)->samplers.table.bo->gpu;
   root.stack_base=stack.gpu;root.stack_stride=stride;root.lanes=lanes.gpu;root.physical_count=physical;
   root.indirect_dimensions=indirect;root.indirect2=indirect2;root.dimensions[0]=width;root.dimensions[1]=height;root.dimensions[2]=depth;
   root.function_count=pipeline->function_count;root.program_count=pipeline->program_count;root.group_count=pipeline->group_count;
   root.control_pc=pipeline->function_count-1;root.traversal_program=pipeline->traversal_program;root.max_trace_depth=pipeline->recursion_depth;root.attribute_size=pipeline->attribute_size;
   if(!indirect2){root.raygen=(struct kk_rt_sbt_region){raygen->deviceAddress,raygen->stride,raygen->size};root.miss=(struct kk_rt_sbt_region){miss->deviceAddress,miss->stride,miss->size};root.hit=(struct kk_rt_sbt_region){hit->deviceAddress,hit->stride,hit->size};root.callable=(struct kk_rt_sbt_region){callable->deviceAddress,callable->stride,callable->size};}
   struct kk_ptr functions=kk_rt_arena(cmd,(uint64_t)pipeline->function_count*sizeof(*pipeline->functions));
   struct kk_ptr programs=kk_rt_arena(cmd,(uint64_t)pipeline->program_count*sizeof(struct kk_rt_program_info));
   struct kk_ptr groups=kk_rt_arena(cmd,(uint64_t)pipeline->group_count*sizeof(*pipeline->groups));
   if(!functions.gpu||!programs.gpu||!groups.gpu)return;
   memcpy(functions.cpu,pipeline->functions,(uint64_t)pipeline->function_count*sizeof(*pipeline->functions));
   memcpy(groups.cpu,pipeline->groups,(uint64_t)pipeline->group_count*sizeof(*pipeline->groups));
   for(uint32_t i=0;i<pipeline->program_count;++i){const struct kk_rt_shader *shader=kk_rt_shader_from_vk(pipeline->programs[i].shader);
      ((struct kk_rt_program_info*)programs.cpu)[i]=(struct kk_rt_program_info){pipeline->programs[i].function_first,pipeline->programs[i].function_count,shader->vk.scratch_size,shader->attribute_size,shader->embedded_sampler_table};}
   root.function_info=functions.gpu;root.program_info=programs.gpu;root.groups=groups.gpu;
   struct kk_ptr args=kk_pool_upload(cmd,&root,sizeof(root),16);if(!args.gpu)return;
   /* Internal RT dispatch changes the physical compute PSO. Invalidate the
    * ordinary compute binding cache through its existing encoder helper. */
   mtl_compute_encoder *encoder=cs_get_compute(cmd);
   KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder,MTL_STAGE_DISPATCH,MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
   uint64_t prior_root=cmd->state.root_addr;
   kk_cmd_bind_root_to_argument_table(cmd,args.gpu);
   mtl_compute_set_pipeline_state(encoder,pipeline->native.pipeline);
   mtl_dispatch_threads(encoder,(struct mtl_size){physical,1,1},(struct mtl_size){32,1,1});
   KK_CMD_BARRIER(cmd, mtl_barrier_after_encoder_stages_with_options, encoder,MTL_STAGE_DISPATCH,MTL_STAGE_DISPATCH, MTL_VISIBILITY_RESOURCE_ALIAS);
   kk_cmd_bind_root_to_argument_table(cmd,prior_root);
}
VKAPI_ATTR void VKAPI_CALL
kk_CmdTraceRaysKHR(VkCommandBuffer cb,const VkStridedDeviceAddressRegionKHR *raygen,const VkStridedDeviceAddressRegionKHR *miss,
                     const VkStridedDeviceAddressRegionKHR *hit,const VkStridedDeviceAddressRegionKHR *callable,uint32_t w,uint32_t h,uint32_t d)
{VK_FROM_HANDLE(kk_cmd_buffer,cmd,cb);kk_rt_trace(cmd,raygen,miss,hit,callable,w,h,d,0,0);}
VKAPI_ATTR void VKAPI_CALL
kk_CmdTraceRaysIndirectKHR(VkCommandBuffer cb,const VkStridedDeviceAddressRegionKHR *raygen,const VkStridedDeviceAddressRegionKHR *miss,
                     const VkStridedDeviceAddressRegionKHR *hit,const VkStridedDeviceAddressRegionKHR *callable,VkDeviceAddress address)
{VK_FROM_HANDLE(kk_cmd_buffer,cmd,cb);kk_rt_trace(cmd,raygen,miss,hit,callable,0,0,0,address,0);}
VKAPI_ATTR void VKAPI_CALL
kk_CmdTraceRaysIndirect2KHR(VkCommandBuffer cb,VkDeviceAddress address)
{VK_FROM_HANDLE(kk_cmd_buffer,cmd,cb);kk_rt_trace(cmd,NULL,NULL,NULL,NULL,0,0,0,0,address);}
