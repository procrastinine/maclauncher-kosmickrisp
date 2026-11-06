/* SPDX-License-Identifier: MIT */
/* The ordinary Vulkan meta builder owns coordinate transformation and typed
 * sampling. This adapter changes its output stage to canonical image stores. */
#include "nir_format_convert.h"

struct kk_standard_3d_blit_lower {
   enum pipe_format format,raw;
   nir_variable *output;
};

static nir_def *
kk_standard_3d_blit_coord(nir_builder *b)
{
   nir_def *origin=nir_load_push_constant(b,3,32,nir_imm_int(b,32),.base=0,.range=64);
   return nir_iadd(b,nir_load_global_invocation_id(b,32),origin);
}

static bool
kk_standard_3d_blit_lower(nir_builder *b,nir_intrinsic_instr *intr,void *data)
{
   struct kk_standard_3d_blit_lower *ctx=data;
   b->cursor=nir_before_instr(&intr->instr);
   if(intr->intrinsic==nir_intrinsic_load_frag_coord) {
      nir_def *xy=nir_fadd_imm(b,nir_u2f32(b,nir_trim_vector(b,kk_standard_3d_blit_coord(b),2)),.5);
      nir_def_replace(&intr->def,nir_vec4(b,nir_channel(b,xy,0),nir_channel(b,xy,1),nir_imm_float(b,0),nir_imm_float(b,1)));
      return true;
   }
   if(intr->intrinsic==nir_intrinsic_load_layer_id) {
      nir_def_replace(&intr->def,nir_channel(b,kk_standard_3d_blit_coord(b),2));return true;
   }
   if(intr->intrinsic!=nir_intrinsic_store_deref)return false;
   nir_variable *var=nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
   if(!var||var->data.mode!=nir_var_shader_out)return false;
   nir_def *packed=nir_format_pack_rgba(b,ctx->format,intr->src[1].ssa);
   packed=nir_pad_vector(b,nir_u2u32(b,packed),4);
   nir_image_deref_store(b,&nir_build_deref_var(b,ctx->output)->def,
      nir_pad_vector(b,kk_standard_3d_blit_coord(b),4),nir_imm_int(b,0),packed,nir_imm_int(b,0),
      .image_dim=GLSL_SAMPLER_DIM_3D,.image_array=false,.format=ctx->raw,
      .src_type=nir_type_uint32,.access=ACCESS_NON_READABLE);
   nir_instr_remove(&intr->instr);return true;
}

static nir_shader *
kk_standard_3d_blit_shader(VkImageType source,VkFormat format,VkFormat raw)
{
   nir_shader *nir=vk_meta_build_color_blit_shader(source,format);
   nir->info.stage=MESA_SHADER_COMPUTE;
   nir->info.workgroup_size[0]=8;nir->info.workgroup_size[1]=8;nir->info.workgroup_size[2]=1;
   nir_variable *output=nir_variable_create(nir,nir_var_uniform,
      glsl_image_type(GLSL_SAMPLER_DIM_3D,false,GLSL_TYPE_UINT),"canonical_destination");
   output->data.descriptor_set=0;output->data.binding=4;
   struct kk_standard_3d_blit_lower ctx={vk_format_to_pipe_format(format),vk_format_to_pipe_format(raw),output};
   nir_shader_intrinsics_pass(nir,kk_standard_3d_blit_lower,nir_metadata_none,&ctx);
   nir_builder b=nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(nir)));
   nir_def *extent=nir_load_push_constant(&b,3,32,nir_imm_int(&b,48),.base=0,.range=64);
   nir_push_if(&b,nir_bany(&b,nir_uge(&b,nir_load_global_invocation_id(&b,32),extent)));
   nir_jump(&b,nir_jump_return);nir_pop_if(&b,NULL);
   nir_remove_dead_variables(nir,nir_var_shader_out,NULL);
   nir_shader_gather_info(nir,nir_shader_get_entrypoint(nir));return nir;
}

static VkResult
kk_standard_3d_blit_pipeline(struct kk_device *dev,VkImageType source,VkFormat format,
   VkFormat raw,VkPipelineLayout *layout,VkPipeline *pipeline)
{
   const struct {uint32_t type;char name[24];VkImageType source;VkFormat format;} key={
      .type=VK_META_OBJECT_KEY_DRIVER_OFFSET,.name="kk-standard-volume-blit",.source=source,.format=format};
   const VkDescriptorSetLayoutBinding binding[]={
      {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding=4,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
   const VkDescriptorSetLayoutCreateInfo descriptors={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,.bindingCount=ARRAY_SIZE(binding),.pBindings=binding};
   const VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,64};
   VkResult result=vk_meta_get_pipeline_layout(&dev->vk,&dev->meta,&descriptors,&push,&key,sizeof(key),layout);
   if(result!=VK_SUCCESS)return result;
   *pipeline=vk_meta_lookup_pipeline(&dev->meta,&key,sizeof(key));if(*pipeline)return VK_SUCCESS;
   const VkPipelineShaderStageNirCreateInfoMESA shader={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA,
      .nir=kk_standard_3d_blit_shader(source,format,raw)};
   const VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.pNext=&shader,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.pName="main"},.layout=*layout};
   result=vk_meta_create_compute_pipeline(&dev->vk,&dev->meta,&info,&key,sizeof(key),pipeline);
   ralloc_free(shader.nir);return result;
}

bool
kk_standard_3d_blit(struct kk_cmd_buffer *cmd,const VkBlitImageInfo2 *info)
{
   VK_FROM_HANDLE(kk_image,dst,info->dstImage);
   if(!kk_standard_3d_copy_enabled(dst))return false;
   VK_FROM_HANDLE(kk_image,src,info->srcImage);
   struct kk_device *dev=kk_cmd_buffer_device(cmd);
   VkPipelineLayout layout;VkPipeline pipeline;VkSampler sampler;
   VkFormat raw=kk_standard_3d_copy_properties(dst).color.view_format;
   VkResult result=kk_standard_3d_blit_pipeline(dev,src->vk.image_type,dst->vk.format,raw,&layout,&pipeline);
   if(result!=VK_SUCCESS){vk_command_buffer_set_error(&cmd->vk,result);return true;}
   result=vk_meta_get_color_blit_sampler(&dev->vk,&dev->meta,info->filter,&sampler);
   if(result!=VK_SUCCESS){vk_command_buffer_set_error(&cmd->vk,result);return true;}
   struct kk_standard_3d_meta_save save;kk_standard_3d_meta_begin(cmd,&save,"meta:sparse volume blit");
   const struct vk_device_dispatch_table *disp=&dev->vk.dispatch_table;VkCommandBuffer handle=kk_cmd_buffer_to_handle(cmd);
   disp->CmdBindPipeline(handle,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
   for(uint32_t r=0;r<info->regionCount;r++) {
      const VkImageBlit2 *region=&info->pRegions[r];
      VkImageView views[2];
      for(unsigned target=0;target<2;target++) {
         const struct kk_image *image=target?dst:src;
         const VkImageSubresourceLayers *sub=target?&region->dstSubresource:&region->srcSubresource;
         const VkImageViewUsageCreateInfo usage={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage=target?VK_IMAGE_USAGE_STORAGE_BIT:VK_IMAGE_USAGE_SAMPLED_BIT};
         const VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.pNext=&usage,
            .flags=VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,.image=kk_image_to_handle((struct kk_image *)image),
            .viewType=image->vk.image_type==VK_IMAGE_TYPE_3D?VK_IMAGE_VIEW_TYPE_3D:
               image->vk.image_type==VK_IMAGE_TYPE_1D?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format=target?raw:image->vk.format,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,sub->mipLevel,1,
               image->vk.image_type==VK_IMAGE_TYPE_3D?0:sub->baseArrayLayer,
               image->vk.image_type==VK_IMAGE_TYPE_3D?1:vk_image_subresource_layer_count(&image->vk,sub)}};
         result=vk_meta_create_image_view(&cmd->vk,&dev->meta,&view,&views[target]);
         if(result!=VK_SUCCESS)break;
      }
      if(result!=VK_SUCCESS){vk_command_buffer_set_error(&cmd->vk,result);break;}
      const VkDescriptorImageInfo images[]={{.sampler=sampler},{.imageView=views[0],.imageLayout=info->srcImageLayout},
         {.imageView=views[1],.imageLayout=info->dstImageLayout}};
      VkWriteDescriptorSet writes[3]={0};const unsigned binding[]={0,1,4};
      const VkDescriptorType type[]={VK_DESCRIPTOR_TYPE_SAMPLER,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
      for(unsigned i=0;i<3;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstBinding=binding[i],.descriptorCount=1,.descriptorType=type[i],.pImageInfo=&images[i]};
      disp->CmdPushDescriptorSetKHR(handle,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,3,writes);
      struct {float transform[8];uint32_t origin[4],extent[4];} push={0};
      VkExtent3D source_extent=vk_image_mip_level_extent(&src->vk,region->srcSubresource.mipLevel);
      const uint32_t dimensions[]={source_extent.width,source_extent.height,source_extent.depth};
      for(unsigned a=0;a<3;a++) {
         int32_t s0=(&region->srcOffsets[0].x)[a],s1=(&region->srcOffsets[1].x)[a];
         int32_t d0=(&region->dstOffsets[0].x)[a],d1=(&region->dstOffsets[1].x)[a];
         bool normalized=a<2||src->vk.image_type==VK_IMAGE_TYPE_3D;
         if(a==2&&!normalized){s0=0;s1=vk_image_subresource_layer_count(&src->vk,&region->srcSubresource);}
         double scale=(double)(s1-s0)/(d1-d0),off=s0-d0*scale;
         if(normalized){scale/=dimensions[a];off/=dimensions[a];}
         push.transform[a<2?a:4]=off;push.transform[a<2?a+2:5]=scale;
         push.origin[a]=MIN2(d0,d1);push.extent[a]=abs(d1-d0);
      }
      disp->CmdPushConstants(handle,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
      disp->CmdDispatch(handle,DIV_ROUND_UP(push.extent[0],8),DIV_ROUND_UP(push.extent[1],8),push.extent[2]);
   }
   kk_standard_3d_meta_end(cmd,&save);return true;
}
