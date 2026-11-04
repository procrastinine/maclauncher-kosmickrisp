/* SPDX-License-Identifier: MIT */
#include "kosmickrisp-rt-groups.h"
#include "kosmickrisp-rt-shader.h"
#include "kk_device.h"
#include "vk_alloc.h"
#include "vk_sampler.h"
#include "vk_meta.h"
#include "util/blob.h"
#include <stdlib.h>
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
struct kk_rt_intern_entry { uint64_t id; size_t size; void *key; };
/* Lazy device-local key interning in the existing meta object cache. No native
 * functions, AS objects or command state lives here. Complete keys resolve
 * equality, independently of hashes and executable-local VFT positions. */
struct kk_rt_intern {
   struct vk_pipeline vk;
   simple_mtx_t mutex;
   uint64_t namespace[2], next;
   struct util_dynarray entries;
};
static void
kk_rt_intern_destroy(struct vk_device *device, struct vk_pipeline *base,
                       const VkAllocationCallbacks *alloc)
{
   struct kk_rt_intern *intern=container_of(base,struct kk_rt_intern,vk);
   util_dynarray_foreach(&intern->entries,struct kk_rt_intern_entry,entry) vk_free(&device->alloc,entry->key);
   util_dynarray_fini(&intern->entries);simple_mtx_destroy(&intern->mutex);
   vk_pipeline_free(device,alloc,base);
}
static const struct vk_pipeline_ops intern_ops={.destroy=kk_rt_intern_destroy};
static struct kk_rt_intern *
kk_rt_intern_get(struct vk_device *device)
{
   struct kk_device *dev=container_of(device,struct kk_device,vk);
   struct {uint32_t type; char name[12]; uint32_t abi;} key={VK_META_OBJECT_KEY_DRIVER_OFFSET,"KKRTINTERN",1};
   VkPipeline handle=vk_meta_lookup_pipeline(&dev->meta,&key,sizeof(key));
   if(!handle){
      struct kk_rt_intern *intern=vk_pipeline_zalloc(device,&intern_ops,VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,0,NULL,sizeof(*intern));
      if(!intern)return NULL;
      simple_mtx_init(&intern->mutex,mtx_plain);intern->entries=UTIL_DYNARRAY_INIT;intern->next=1;
      arc4random_buf(intern->namespace,sizeof(intern->namespace));
      handle=(VkPipeline)vk_meta_cache_object(device,&dev->meta,&key,sizeof(key),VK_OBJECT_TYPE_PIPELINE,(uint64_t)vk_pipeline_to_handle(&intern->vk));
   }
   return container_of(vk_pipeline_from_handle(handle),struct kk_rt_intern,vk);
}
static bool
kk_rt_group_key(VkRayTracingShaderGroupTypeKHR type,const struct vk_shader *const *shaders,uint32_t count,struct blob *key)
{
   blob_write_uint32(key,type);blob_write_uint32(key,count);
   for(uint32_t i=0;i<count;++i){
      const struct kk_rt_shader *shader=kk_rt_shader_from_vk(shaders[i]);if(!shader)return false;
      blob_write_uint32(key,shaders[i]->stage);blob_write_uint32(key,shader->function_count);
      blob_write_uint32(key,shader->embedded_sampler_count);
      if(shader->embedded_sampler_count)blob_write_bytes(key,shader->embedded_sampler_states,
         (uint64_t)shader->embedded_sampler_count*sizeof(struct vk_sampler_state));
      for(uint32_t f=0;f<shader->function_count;++f){
         blob_write_uint32(key,(shader->functions[f].requires_msl41 | (shader->functions[f].uses_atomic64_locks << 1)));
         size_t size=strlen(shader->functions[f].source)+1;blob_write_uint64(key,size);blob_write_bytes(key,shader->functions[f].source,size);
      }
   }
   return !key->out_of_memory;
}
static VkResult
kk_rt_intern_group(struct vk_device *device, VkRayTracingShaderGroupTypeKHR type,
                    const struct vk_shader *const *shaders,uint32_t count,bool create,struct kk_rt_group_token *token)
{
   struct kk_rt_intern *intern=kk_rt_intern_get(device);if(!intern)return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct blob key;blob_init(&key);if(!kk_rt_group_key(type,shaders,count,&key)){blob_finish(&key);return VK_ERROR_OUT_OF_HOST_MEMORY;}
   uint64_t id=0;simple_mtx_lock(&intern->mutex);
   util_dynarray_foreach(&intern->entries,struct kk_rt_intern_entry,entry)
      if(entry->size==key.size&&!memcmp(entry->key,key.data,key.size)){id=entry->id;break;}
   if(!id&&create&&intern->next!=UINT64_MAX){
      void *copy=vk_alloc(&device->alloc,key.size,8,VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if(copy){
         struct kk_rt_intern_entry *entry=util_dynarray_grow(&intern->entries,struct kk_rt_intern_entry,1);
         if(entry){memcpy(copy,key.data,key.size);id=intern->next++;*entry=(struct kk_rt_intern_entry){id,key.size,copy};}
         else vk_free(&device->alloc,copy);
      }
   }
   *token=(struct kk_rt_group_token){{intern->namespace[0],intern->namespace[1]},id,type,1};
   simple_mtx_unlock(&intern->mutex);blob_finish(&key);return id?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;
}
static int compare_groups(const void *a,const void *b){uint64_t x=((const struct kk_rt_group_entry*)a)->token.group,y=((const struct kk_rt_group_entry*)b)->token.group;return(x>y)-(x<y);}
VkResult
kk_rt_pipeline_groups(struct vk_device *device,const struct vk_pipeline *common,const VkAllocationCallbacks *alloc,struct kk_rt_group_entry **out)
{
   *out=NULL;uint32_t count=vk_pipeline_get_rt_group_count(common);if(!count)return VK_SUCCESS;
   uint64_t bytes;if(!kk_rt_u64_mul(count,sizeof(**out),&bytes)||bytes>SIZE_MAX)return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_rt_group_entry *entries=vk_zalloc2(&device->alloc,alloc,bytes,8,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);if(!entries)return VK_ERROR_OUT_OF_HOST_MEMORY;
   for(uint32_t g=0;g<count;++g){
      struct vk_rt_group_shader_info group;vk_pipeline_get_rt_group(common,g,&group);
      VkResult result=kk_rt_intern_group(device,group.type,group.shaders,group.shader_count,true,&entries[g].token);
      if(result!=VK_SUCCESS){vk_free2(&device->alloc,alloc,entries);return result;}
      entries[g].general=entries[g].closest_hit=entries[g].any_hit=entries[g].intersection=KK_RT_STOP;
      for(uint32_t s=0;s<group.shader_count;++s){
         uint32_t program=KK_RT_STOP;
         for(uint32_t p=0;p<vk_pipeline_get_rt_stage_count(common);++p)if(vk_pipeline_get_rt_stage(common,p)==group.shaders[s]){program=p;break;}
         if(program==KK_RT_STOP){vk_free2(&device->alloc,alloc,entries);return VK_ERROR_INVALID_SHADER_NV;}
         switch(group.shaders[s]->stage){
         case MESA_SHADER_ANY_HIT: entries[g].any_hit=program;break;
         case MESA_SHADER_CLOSEST_HIT:entries[g].closest_hit=program;break;
         case MESA_SHADER_INTERSECTION:entries[g].intersection=program;break;
         default:entries[g].general=program;break;
         }
      }
   }
   qsort(entries,count,sizeof(*entries),compare_groups);*out=entries;return VK_SUCCESS;
}
/* Handle reads cannot report allocation failure through the common void
 * callback. Compare the retained complete key in place, without allocation. */
static bool
kk_rt_group_matches(const struct kk_rt_intern_entry *entry,
                    VkRayTracingShaderGroupTypeKHR type,
                    const struct vk_shader *const *shaders,uint32_t count)
{
   struct blob_reader key;blob_reader_init(&key,entry->key,entry->size);
   if(blob_read_uint32(&key)!=type||blob_read_uint32(&key)!=count)return false;
   for(uint32_t i=0;i<count;++i){
      const struct kk_rt_shader *shader=kk_rt_shader_from_vk(shaders[i]);
      if(!shader||blob_read_uint32(&key)!=shaders[i]->stage||
         blob_read_uint32(&key)!=shader->function_count)return false;
      if(blob_read_uint32(&key)!=shader->embedded_sampler_count)return false;
      uint64_t sampler_bytes=(uint64_t)shader->embedded_sampler_count*sizeof(struct vk_sampler_state);
      const void *samplers=blob_read_bytes(&key,sampler_bytes);
      if(sampler_bytes&&(!samplers||memcmp(samplers,shader->embedded_sampler_states,sampler_bytes)))return false;
      for(uint32_t f=0;f<shader->function_count;++f){
         if(blob_read_uint32(&key)!=(shader->functions[f].requires_msl41 | (shader->functions[f].uses_atomic64_locks << 1)))return false;
         size_t size=strlen(shader->functions[f].source)+1;
         if(blob_read_uint64(&key)!=size)return false;
         const void *bytes=blob_read_bytes(&key,size);
         if(!bytes||memcmp(bytes,shader->functions[f].source,size))return false;
      }
   }
   return !key.overrun&&key.current==key.end;
}
void
kk_rt_write_group(struct vk_device *device,VkRayTracingShaderGroupTypeKHR type,const struct vk_shader **shaders,uint32_t count,void *output)
{
   struct kk_rt_intern *intern=kk_rt_intern_get(device);
   assert(intern);uint64_t id=0;
   simple_mtx_lock(&intern->mutex);
   util_dynarray_foreach(&intern->entries,struct kk_rt_intern_entry,entry)
      if(kk_rt_group_matches(entry,type,shaders,count)){id=entry->id;break;}
   struct kk_rt_group_token token={{intern->namespace[0],intern->namespace[1]},id,type,1};
   simple_mtx_unlock(&intern->mutex);
   /* Publication always follows successful interning; device lifetime retains
    * every identity while common pipeline/library shader references live. */
   assert(id);memcpy(output,&token,sizeof(token));
}
