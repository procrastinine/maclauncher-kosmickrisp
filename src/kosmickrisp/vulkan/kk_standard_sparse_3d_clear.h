/* SPDX-License-Identifier: MIT */
/* Included in the owned meta implementation; pipeline/layout lifetimes and
 * state preservation use the ordinary Vulkan meta cache. */
#include "nir_builder.h"
#include "vk_pipeline.h"
#include "util/format/u_format.h"

static nir_shader *
kk_standard_3d_clear_shader(VkFormat format)
{
   nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,NULL,"kk-volume-clear");
   b.shader->info.workgroup_size[0]=8;b.shader->info.workgroup_size[1]=8;b.shader->info.workgroup_size[2]=1;
   nir_variable *image=nir_variable_create(b.shader,nir_var_uniform,
      glsl_image_type(GLSL_SAMPLER_DIM_3D,false,GLSL_TYPE_UINT),"volume");
   image->data.descriptor_set=0;image->data.binding=0;
   nir_def *xyz=nir_load_global_invocation_id(&b,32);
   nir_def *extent=nir_load_push_constant(&b,3,32,nir_imm_int(&b,0),.base=0,.range=32);
   nir_push_if(&b,nir_ball(&b,nir_ult(&b,xyz,extent)));
   nir_def *color=nir_load_push_constant(&b,4,32,nir_imm_int(&b,16),.base=0,.range=32);
   nir_image_deref_store(&b,&nir_build_deref_var(&b,image)->def,nir_pad_vector(&b,xyz,4),
      nir_imm_int(&b,0),color,nir_imm_int(&b,0),.image_dim=GLSL_SAMPLER_DIM_3D,
      .image_array=false,.format=vk_format_to_pipe_format(format),
      .src_type=nir_type_uint32,.access=ACCESS_NON_READABLE);
   nir_pop_if(&b,NULL);
   return b.shader;
}

static VkResult
kk_standard_3d_clear_pipeline(struct kk_device *dev,VkFormat format,
   VkPipelineLayout *layout,VkPipeline *pipeline)
{
   const struct {uint32_t type;char name[24];VkFormat format;} key={
      .type=VK_META_OBJECT_KEY_DRIVER_OFFSET,.name="kk-standard-volume-clear",.format=format,
   };
   const VkDescriptorSetLayoutBinding binding={.binding=0,
      .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
   const VkDescriptorSetLayoutCreateInfo descriptors={
      .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount=1,.pBindings=&binding,
   };
   const VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,32};
   VkResult result=vk_meta_get_pipeline_layout(&dev->vk,&dev->meta,&descriptors,&push,
      &key,sizeof(key),layout);
   if(result!=VK_SUCCESS)return result;
   *pipeline=vk_meta_lookup_pipeline(&dev->meta,&key,sizeof(key));
   if(*pipeline)return VK_SUCCESS;
   const VkPipelineShaderStageNirCreateInfoMESA shader={
      .sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir=kk_standard_3d_clear_shader(format),
   };
   const VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.pNext=&shader,
         .stage=VK_SHADER_STAGE_COMPUTE_BIT,.pName="main"},.layout=*layout,
   };
   result=vk_meta_create_compute_pipeline(&dev->vk,&dev->meta,&info,&key,sizeof(key),pipeline);
   ralloc_free(shader.nir);return result;
}

void
kk_standard_3d_clear(struct kk_cmd_buffer *cmd,struct kk_image *image,
   VkImageLayout image_layout,const VkClearColorValue *color,uint32_t count,
   const VkImageSubresourceRange *ranges)
{
   assert(kk_image_is_standard_3d(image));
   struct kk_device *dev=kk_cmd_buffer_device(cmd);
   VkPipelineLayout layout;VkPipeline pipeline;
   VkFormat raw=kk_standard_3d_copy_properties(image).color.view_format;
   VkResult result=kk_standard_3d_clear_pipeline(dev,raw,&layout,&pipeline);
   if(result!=VK_SUCCESS){vk_command_buffer_set_error(&cmd->vk,result);return;}
   uint32_t push[8]={0};util_format_pack_rgba(vk_format_to_pipe_format(image->vk.format),push+4,color,1);
   struct kk_standard_3d_meta_save save;
   kk_standard_3d_meta_begin(cmd,&save,"meta:sparse volume clear");
   const struct vk_device_dispatch_table *disp=&dev->vk.dispatch_table;
   VkCommandBuffer handle=kk_cmd_buffer_to_handle(cmd);
   disp->CmdBindPipeline(handle,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
   for(uint32_t r=0;r<count;r++)for(uint32_t l=0;l<vk_image_subresource_level_count(&image->vk,&ranges[r]);l++){
      uint32_t level=ranges[r].baseMipLevel+l;
      const VkImageViewUsageCreateInfo usage={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
         .usage=VK_IMAGE_USAGE_STORAGE_BIT};
      const VkImageViewCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .pNext=&usage,.flags=VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
         .image=kk_image_to_handle(image),.viewType=VK_IMAGE_VIEW_TYPE_3D,.format=raw,
         .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,1,0,1}};
      VkImageView view;
      result=vk_meta_create_image_view(&cmd->vk,&dev->meta,&info,&view);
      if(result!=VK_SUCCESS){vk_command_buffer_set_error(&cmd->vk,result);break;}
      const VkDescriptorImageInfo target={.imageView=view,.imageLayout=image_layout};
      const VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&target};
      disp->CmdPushDescriptorSetKHR(handle,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&write);
      VkExtent3D extent=vk_image_mip_level_extent(&image->vk,level);
      push[0]=extent.width;push[1]=extent.height;push[2]=extent.depth;
      disp->CmdPushConstants(handle,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
      disp->CmdDispatch(handle,DIV_ROUND_UP(extent.width,8),DIV_ROUND_UP(extent.height,8),extent.depth);
   }
   kk_standard_3d_meta_end(cmd,&save);
   return;
}
