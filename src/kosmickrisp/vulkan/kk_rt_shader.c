/* SPDX-License-Identifier: MIT */
#include "kosmickrisp-rt-shader.h"
#include "kosmickrisp-rt-cps.h"
#include "kk_rt_stack.h"
#include "kk_device.h"
#include "kk_physical_device.h"
#include "kk_shader.h"
#include "kk_atomic64.h"
#include "kk_perf.h"
#include "kk_descriptor_set.h"
#include "vk_sampler.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "util/blob.h"
#include "util/mesa-blake3.h"
#include "nir_serialize.h"
#include "vk_util.h"

#define KK_RT_BINARY_MAGIC 0x4b525453u
#define KK_RT_BINARY_ABI 5u
static const struct vk_shader_ops kk_rt_shader_ops;

const struct kk_rt_shader *
kk_rt_shader_from_vk(const struct vk_shader *shader)
{
   if (!shader || shader->ops != &kk_rt_shader_ops)
      return NULL;
   return container_of(shader, const struct kk_rt_shader, vk);
}

static void
kk_rt_shader_destroy(struct vk_device *device, struct vk_shader *base,
                       const VkAllocationCallbacks *alloc)
{
   struct kk_rt_shader *shader = container_of(base, struct kk_rt_shader, vk);
   for (uint32_t i = 0; i < shader->function_count; ++i) {
      struct kk_rt_function *f = &shader->functions[i];
      mtl_release(f->descriptor);
      mtl_release(f->library);
      ralloc_free(f->name);
      ralloc_free(f->source);
   }
   vk_free2(&device->alloc, alloc, shader->embedded_sampler_states);
   vk_free2(&device->alloc, alloc, shader->functions);
   vk_shader_free(device, alloc, base);
}

static bool
kk_rt_shader_serialize(struct vk_device *device, const struct vk_shader *base,
                         struct blob *blob)
{
   const struct kk_rt_shader *shader = kk_rt_shader_from_vk(base);
   blob_write_uint32(blob, KK_RT_BINARY_MAGIC);
   blob_write_uint32(blob, KK_RT_BINARY_ABI);
   blob_write_uint32(blob, base->stage);
   blob_write_uint32(blob, shader->function_count);
   blob_write_uint64(blob, base->scratch_size);
   blob_write_uint64(blob, base->stack_size);
   blob_write_uint32(blob, base->ray_queries);
   blob_write_uint64(blob, shader->payload_size);
   blob_write_uint64(blob, shader->attribute_size);
   blob_write_uint32(blob,shader->push_size);
   blob_write_uint32(blob,shader->descriptor_heap);
   blob_write_uint32(blob,shader->embedded_sampler_count);
   if(shader->embedded_sampler_count)blob_write_bytes(blob,shader->embedded_sampler_states,
      (uint64_t)shader->embedded_sampler_count*sizeof(struct vk_sampler_state));
   for (uint32_t i = 0; i < shader->function_count; ++i) {
      const struct kk_rt_function *f = &shader->functions[i];
      size_t n = strlen(f->name) + 1, s = strlen(f->source) + 1;
      if (n > UINT32_MAX || s > UINT32_MAX)
         return false;
      blob_write_uint32(blob, n);
      blob_write_uint32(blob, s);
      blob_write_uint32(blob, f->requires_msl41 | (f->uses_atomic64_locks << 1) |
         (f->uses_sampler_emulation << 2) | (f->requires_sampler_emulation << 3));
      blob_write_bytes(blob, f->name, n);
      blob_write_bytes(blob, f->source, s);
   }
   return !blob->out_of_memory;
}

static VkResult
kk_rt_executable_properties(struct vk_device *device,const struct vk_shader *base,uint32_t *count,VkPipelineExecutablePropertiesKHR *properties)
{
   if(!properties){*count=1;return VK_SUCCESS;}
   if(!*count){return VK_INCOMPLETE;}
   *count=1;properties[0].stages=mesa_to_vk_shader_stage(base->stage);
   snprintf(properties[0].name,sizeof(properties[0].name),"RT stage continuations");
   snprintf(properties[0].description,sizeof(properties[0].description),"Separate entry and resume functions for the ray tracing stage");
   properties[0].subgroupSize=32;return VK_SUCCESS;
}
static VkResult
kk_rt_executable_statistics(struct vk_device *device,const struct vk_shader *base,uint32_t index,uint32_t *count,VkPipelineExecutableStatisticKHR *statistics)
{
   const struct kk_rt_shader *shader=kk_rt_shader_from_vk(base);const uint64_t values[]={base->scratch_size,shader->payload_size,shader->attribute_size};
   const char *names[]={"Continuation scratch bytes","Payload interface bytes","Hit attribute interface bytes"};
   if(!statistics){*count=3;return VK_SUCCESS;}uint32_t n=MIN2(*count,3);*count=n;
   for(uint32_t i=0;i<n;++i){snprintf(statistics[i].name,sizeof(statistics[i].name),"%s",names[i]);snprintf(statistics[i].description,sizeof(statistics[i].description),"Compiled RT interface or continuation storage size");statistics[i].format=VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR;statistics[i].value.u64=values[i];}
   return n==3?VK_SUCCESS:VK_INCOMPLETE;
}
static VkResult
kk_rt_executable_representations(struct vk_device *device,const struct vk_shader *base,uint32_t index,uint32_t *count,VkPipelineExecutableInternalRepresentationKHR *representations)
{
   const struct kk_rt_shader *shader=kk_rt_shader_from_vk(base);if(!representations){*count=shader->function_count;return VK_SUCCESS;}
   uint32_t n=MIN2(*count,shader->function_count);*count=n;VkResult result=n==shader->function_count?VK_SUCCESS:VK_INCOMPLETE;
   for(uint32_t i=0;i<n;++i){const struct kk_rt_function *f=&shader->functions[i];size_t size=strlen(f->source)+1;
      snprintf(representations[i].name,sizeof(representations[i].name),"%s",f->name);snprintf(representations[i].description,sizeof(representations[i].description),"Metal source for separate RT function");representations[i].isText=VK_TRUE;
      if(representations[i].pData){if(representations[i].dataSize<size)result=VK_INCOMPLETE;else memcpy(representations[i].pData,f->source,size);}
      representations[i].dataSize=size;
   }return result;
}

static const struct vk_shader_ops kk_rt_shader_ops = {
   .destroy = kk_rt_shader_destroy,
   .serialize = kk_rt_shader_serialize,
   .get_executable_properties=kk_rt_executable_properties,
   .get_executable_statistics=kk_rt_executable_statistics,
   .get_executable_internal_representations=kk_rt_executable_representations,
};

static bool
kk_rt_function_compile(struct kk_device *device, struct kk_rt_function *f)
{
   struct kk_physical_device *pdev = kk_device_physical(device);
   if(f->requires_msl41 && pdev->info.msl_version < MTL_LANGUAGE_VERSION_4_1)
      return false;
   const struct msl_compile_data language={.requires_msl41=f->requires_msl41,
      .uses_atomic64_locks=f->uses_atomic64_locks,
      .uses_sampler_emulation=f->uses_sampler_emulation,
      .requires_sampler_emulation=f->requires_sampler_emulation};
   f->library = mtl_new_library(device->mtl_compiler_handle, f->source,
      kk_msl_language_version(&language,pdev->info.msl_version),
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   if (!f->library)
      return false;
   return kk_msl_function(device, &language, f->library, f->name,
                          &f->descriptor) == VK_SUCCESS;
}

static VkResult
kk_rt_compile_one(struct vk_device *device, nir_shader *nir,
                    const VkAllocationCallbacks *alloc, struct vk_shader **out,
                    kk_rt_compile_part compile_part, bool internal)
{
   struct kk_device *dev = container_of(device, struct kk_device, vk);
   struct kk_rt_shader *shader = vk_shader_zalloc(device, &kk_rt_shader_ops,
      nir->info.stage, alloc, sizeof(*shader));
   if (!shader) {
      ralloc_free(nir);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   /* Names derive from the complete lowered shader, not a library-local or
    * link-local stage ordinal. Imports keep the common shader objects. */
   struct blob key;
   blob_init(&key);
   nir_serialize(&key, nir, false);
   blake3_hash digest;
   if (key.out_of_memory) {
      blob_finish(&key);
      ralloc_free(nir);
      kk_rt_shader_destroy(device, &shader->vk, alloc);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   _mesa_blake3_compute(key.data, key.size, digest);
   blob_finish(&key);
   char hex[65];
   for (unsigned i = 0; i < sizeof(digest); ++i)
      snprintf(hex + 2 * i, 3, "%02x", digest[i]);
   shader->vk.ray_queries = nir->info.ray_queries;
   struct kk_rt_shader_parts parts;
   if (!kk_rt_split_shader_mode(nir, NULL, &parts, internal)) {
      ralloc_free(nir);
      kk_rt_shader_destroy(device, &shader->vk, alloc);
      return VK_ERROR_INVALID_SHADER_NV;
   }
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   uint64_t count = (uint64_t)parts.resume_count + 1;
   if (count > UINT32_MAX || count > SIZE_MAX / sizeof(*shader->functions))
      goto finish_parts;
   shader->functions = vk_zalloc2(&device->alloc, alloc, count * sizeof(*shader->functions),
      8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!shader->functions)
      goto finish_parts;
   shader->function_count = count;
   shader->vk.scratch_size = parts.scratch_size;
   shader->vk.stack_size = sizeof(struct kk_rt_cps_frame) + (uint64_t)parts.scratch_size;
   shader->payload_size = parts.payload_size;
   shader->attribute_size = parts.hit_attribute_size;
   for (uint32_t i = 0; i < shader->function_count; ++i) {
      nir_shader *part = i ? parts.resumes[i - 1] : parts.entry;
      nir_function_impl *impl = nir_shader_get_entrypoint(part);
      ralloc_free((void *)impl->function->name);
      impl->function->name = ralloc_asprintf(impl->function, "kk_rt_%s_%u", hex, i);
      struct msl_compile_data msl = {0};
      compile_part(dev, part, NULL, &msl);
      if (i) parts.resumes[i - 1] = NULL; else parts.entry = NULL;
      struct kk_rt_function *f = &shader->functions[i];
      f->name = msl.entrypoint_name;
      f->source = msl.code;
      f->requires_msl41=msl.requires_msl41;
      f->uses_atomic64_locks=msl.uses_atomic64_locks;
      f->uses_sampler_emulation=msl.uses_sampler_emulation;
      f->requires_sampler_emulation=msl.requires_sampler_emulation;
      if (!f->name || !f->source)
         goto finish_parts;
      if (!kk_rt_function_compile(dev, f)) {
         result = VK_ERROR_INVALID_SHADER_NV;
         goto finish_parts;
      }
   }
   *out = &shader->vk;
   result = VK_SUCCESS;
finish_parts:
   ralloc_free(parts.entry);
   for (uint32_t i = 0; i < parts.resume_count; ++i)
      ralloc_free(parts.resumes[i]);
   ralloc_free(parts.resumes);
   if (result != VK_SUCCESS)
      kk_rt_shader_destroy(device, &shader->vk, alloc);
   return result;
}

static VkResult
kk_rt_embedded_samplers(struct vk_device *device,struct kk_rt_shader *shader,
                        uint32_t count,const struct vk_sampler_state *states,
                        const VkAllocationCallbacks *alloc)
{
   if(!count)return VK_SUCCESS;
   uint64_t bytes;
   if(!states||count>device->physical->properties.maxDescriptorHeapEmbeddedSamplers||
      !kk_rt_u64_mul(count,sizeof(*states),&bytes)||bytes>SIZE_MAX)
      return VK_ERROR_INVALID_SHADER_NV;
   shader->embedded_sampler_states=vk_alloc2(&device->alloc,alloc,bytes,8,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if(!shader->embedded_sampler_states)return VK_ERROR_OUT_OF_HOST_MEMORY;
   memcpy(shader->embedded_sampler_states,states,bytes);
   shader->embedded_sampler_count=count;
   return kk_heap_embedded_sampler_table(container_of(device,struct kk_device,vk),
      count,states,&shader->embedded_sampler_table);
}

VkResult
kk_rt_compile_lowered(struct vk_device *device, uint32_t count, nir_shader **shaders, const struct vk_shader_compile_info *infos, uint32_t heap_push_size,
                        const VkAllocationCallbacks *alloc, struct vk_shader **out,
                        kk_rt_compile_part compile_part)
{
   memset(out, 0, (uint64_t)count * sizeof(*out));
   for (uint32_t i = 0; i < count; ++i) {
      VkResult result = kk_rt_compile_one(device, shaders[i], alloc, &out[i], compile_part, false);
      if (result == VK_SUCCESS) {
         struct kk_rt_shader *rt=container_of(out[i],struct kk_rt_shader,vk);
         result=kk_rt_embedded_samplers(device,rt,infos[i].embedded_sampler_count,
            infos[i].embedded_samplers,alloc);
         rt->descriptor_heap=(infos[i].flags&VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT)!=0;
         if (rt->descriptor_heap) rt->push_size = heap_push_size;
         for(uint32_t p=0;p<infos[i].push_constant_range_count;++p)
            if(infos[i].push_constant_ranges[p].stageFlags&mesa_to_vk_shader_stage(infos[i].stage))
               rt->push_size=MAX2(rt->push_size,infos[i].push_constant_ranges[p].offset+infos[i].push_constant_ranges[p].size);
      }
      if (result != VK_SUCCESS) {
         if(out[i])kk_rt_shader_destroy(device,out[i],alloc);
         for (uint32_t j = 0; j < i; ++j)
            kk_rt_shader_destroy(device, out[j], alloc);
         for (uint32_t j = i + 1; j < count; ++j)
            ralloc_free(shaders[j]);
         memset(out, 0, count * sizeof(*out));
         return result;
      }
   }
   return VK_SUCCESS;
}

bool
kk_rt_shader_binary(const struct blob_reader *blob)
{
   uint32_t magic;
   if ((size_t)(blob->end - blob->current) < sizeof(magic))
      return false;
   memcpy(&magic, blob->current, sizeof(magic));
   return magic == KK_RT_BINARY_MAGIC;
}

VkResult
kk_rt_shader_deserialize(struct vk_device *device, struct blob_reader *blob,
                           const VkAllocationCallbacks *alloc, struct vk_shader **out)
{
   *out = NULL;
   uint32_t magic = blob_read_uint32(blob), abi = blob_read_uint32(blob);
   mesa_shader_stage stage = blob_read_uint32(blob);
   uint32_t count = blob_read_uint32(blob);
   uint64_t function_bytes;
   if (magic != KK_RT_BINARY_MAGIC || abi != KK_RT_BINARY_ABI || !count ||
       !mesa_shader_stage_is_rt(stage) ||
       !kk_rt_u64_mul(count, sizeof(struct kk_rt_function), &function_bytes) || function_bytes > SIZE_MAX)
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   struct kk_rt_shader *shader = vk_shader_zalloc(device, &kk_rt_shader_ops, stage, alloc, sizeof(*shader));
   if (!shader)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   shader->vk.scratch_size = blob_read_uint64(blob);
   shader->vk.stack_size = blob_read_uint64(blob);
   shader->vk.ray_queries = blob_read_uint32(blob);
   shader->payload_size = blob_read_uint64(blob);
   shader->attribute_size = blob_read_uint64(blob);
   shader->push_size=blob_read_uint32(blob);
   shader->descriptor_heap=blob_read_uint32(blob)!=0;
   if (blob->overrun || shader->vk.scratch_size > UINT32_MAX - 16ull ||
       shader->vk.stack_size != sizeof(struct kk_rt_cps_frame) + shader->vk.scratch_size ||
       shader->payload_size > UINT32_MAX || shader->attribute_size > UINT32_MAX ||
       shader->push_size > KK_MAX_PUSH_SIZE ||
       count > (uint64_t)(blob->end - blob->current) / 14) {
      kk_rt_shader_destroy(device, &shader->vk, alloc);
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   }
   uint32_t sampler_count=blob_read_uint32(blob);uint64_t sampler_bytes;
   if(blob->overrun||!kk_rt_u64_mul(sampler_count,sizeof(struct vk_sampler_state),&sampler_bytes)||
      sampler_bytes>(uint64_t)(blob->end-blob->current)){
      kk_rt_shader_destroy(device,&shader->vk,alloc);
      return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   }
   const struct vk_sampler_state *samplers=blob_read_bytes(blob,sampler_bytes);
   VkResult sampler_result=kk_rt_embedded_samplers(device,shader,sampler_count,samplers,alloc);
   if(sampler_result!=VK_SUCCESS){kk_rt_shader_destroy(device,&shader->vk,alloc);return sampler_result;}
   shader->functions = vk_zalloc2(&device->alloc, alloc, function_bytes,
      8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!shader->functions) {
      kk_rt_shader_destroy(device, &shader->vk, alloc);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   shader->function_count = count;
   VkResult result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
   for (uint32_t i = 0; i < count; ++i) {
      uint32_t n = blob_read_uint32(blob), s = blob_read_uint32(blob);
      uint32_t flags = blob_read_uint32(blob);
      if ((flags & ~15u) || ((flags & 8u) && !(flags & 4u))) goto fail;
      shader->functions[i].requires_msl41 = flags & 1u;
      shader->functions[i].uses_atomic64_locks = flags & 2u;
      shader->functions[i].uses_sampler_emulation = flags & 4u;
      shader->functions[i].requires_sampler_emulation = flags & 8u;
      if (!n || !s || blob->overrun || (uint64_t)n + s > (uint64_t)(blob->end - blob->current))
         goto fail;
      const char *name = blob_read_bytes(blob, n), *source = blob_read_bytes(blob, s);
      if (name[n - 1] || source[s - 1] || memchr(name, 0, n - 1) || memchr(source, 0, s - 1))
         goto fail;
      shader->functions[i].name = ralloc_strdup(NULL, name);
      shader->functions[i].source = ralloc_strdup(NULL, source);
      if (!shader->functions[i].name || !shader->functions[i].source) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }
      if (!kk_rt_function_compile(container_of(device, struct kk_device, vk), &shader->functions[i])) {
         result = VK_ERROR_INVALID_SHADER_NV;
         goto fail;
      }
   }
   *out = &shader->vk;
   return VK_SUCCESS;
fail:
   kk_rt_shader_destroy(device, &shader->vk, alloc);
   return result;
}

nir_shader *kk_rt_build_traversal(void *mem_ctx);
VkResult kk_rt_compile_traversal(struct vk_device *device, const VkAllocationCallbacks *alloc, struct vk_shader **out, kk_rt_compile_part compile_part)
{
   nir_shader *nir=kk_rt_build_traversal(NULL);
   return kk_rt_compile_one(device,nir,alloc,out,compile_part,true);
}
