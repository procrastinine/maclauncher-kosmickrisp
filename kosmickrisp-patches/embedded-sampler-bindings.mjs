// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'embedded-sampler-bindings';
export const sourceFiles = ['kosmickrisp-patches/embedded-sampler-runtime.h'];
export function apply({edit, edits}) {
  const v = (file, apply) => edit('src/kosmickrisp/vulkan/' + file, apply);
  edits.set('src/kosmickrisp/vulkan/kk_embedded_sampler_runtime.h',
    fs.readFileSync(new URL('./embedded-sampler-runtime.h', import.meta.url), 'utf8'));
  v('kk_cmd_buffer.h', s => s
    .replace('      struct { uint64_t heap_resource, heap_sampler; };', `      struct {
         uint64_t heap_resource, heap_sampler;
         uint64_t heap_embedded_samplers[MESA_ALL_SHADER_STAGES];
      };`)
    .replace('struct kk_descriptor_state {', `static_assert(2u + MESA_ALL_SHADER_STAGES <= KK_MAX_SETS,
              "embedded stage bindings fit the existing heap root union");
static_assert(sizeof(((struct kk_root_descriptor_table *)0)->sets) ==
                 KK_MAX_SETS * sizeof(uint64_t), "unchanged root union extent");

struct kk_descriptor_state {`)
    .replace('   bool descriptor_heap;',
      '   bool descriptor_heap;\n   uint64_t heap_embedded_samplers[MESA_ALL_SHADER_STAGES];')
    .replace('      snapshot->heap_sampler = desc->heap_sampler;', `      snapshot->heap_sampler = desc->heap_sampler;
      memcpy(snapshot->heap_embedded_samplers, desc->heap_embedded_samplers,
             sizeof(snapshot->heap_embedded_samplers));`));
  v('kk_shader.h', s => s
    .replace('struct kk_shader {', '#define KK_EMBEDDED_SAMPLER_BINDING_SCHEMA 1u\nstruct vk_sampler_state;\n\nstruct kk_shader {')
    .replace('   struct vk_shader vk;', `   struct vk_shader vk;
   /* Canonical states may be serialized; these CPU/GPU pointers never are. */
   uint32_t embedded_sampler_count;
   struct vk_sampler_state *embedded_sampler_states;
   uint64_t embedded_sampler_table;`)
    .replace('bool kk_nir_prepare_descriptor_heaps(', `VkResult kk_shader_embedded_samplers_init(struct kk_device *dev,
   struct kk_shader *shader, uint32_t count, const struct vk_sampler_state *states);
void kk_shader_embedded_samplers_finish(struct kk_device *dev,
   struct kk_shader *shader);

bool kk_nir_prepare_descriptor_heaps(`));
  v('kk_descriptor_set.h', s => s.replace('VkResult kk_heap_sampler_state(struct kk_device *dev,',
    `VkResult kk_heap_embedded_sampler_table(struct kk_device *dev,
   uint32_t count, const struct vk_sampler_state *states, uint64_t *gpu_address);
VkResult kk_heap_sampler_state(struct kk_device *dev,`));
  v('kk_descriptor_heap_api.h', s => s.replace('VKAPI_ATTR VkResult VKAPI_CALL\nkk_RegisterCustomBorderColorEXT',
    `VkResult
kk_heap_embedded_sampler_table(struct kk_device *dev, uint32_t count,
   const struct vk_sampler_state *states, uint64_t *gpu_address)
{
   *gpu_address = 0;
   if (!count) return VK_SUCCESS;
   if (!states || count > dev->vk.physical->properties.maxDescriptorHeapEmbeddedSamplers ||
       count > KK_HEAP_METADATA_PAGE_SIZE / sizeof(uint64_t))
      return vk_error(dev, VK_ERROR_UNKNOWN);
   uint64_t *addresses = vk_alloc(&dev->vk.alloc, count * sizeof(*addresses), 8,
                                 VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!addresses) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < count; ++i) {
      struct kk_heap_sampler *sampler;
      result = kk_heap_sampler_locked(dev, &states[i], &sampler);
      if (result != VK_SUCCESS) break;
      result = kk_heap_sampler_metadata_locked(dev, sampler);
      if (result != VK_SUCCESS) break;
      addresses[i] = sampler->gpu_address;
   }
   if (result == VK_SUCCESS) {
      if (count == 1)
         *gpu_address = addresses[0];
      else
         result = kk_heap_metadata_alloc_locked(dev, addresses,
            count * sizeof(*addresses), 8, gpu_address);
   }
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   vk_free(&dev->vk.alloc, addresses);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_RegisterCustomBorderColorEXT`));
  v('kk_descriptor_heap_nir.h', s => s.replace(
    '         nir_def *address = nir_imm_int64(b, ctx->embedded[tex->sampler_index]);',
    `         unsigned offset = kk_root_descriptor_offset(heap_embedded_samplers) +
                           b->shader->info.stage * sizeof(uint64_t);
         nir_def *table = load_root(b, 1, 64, nir_imm_int(b, offset), 8);
         nir_def *address = ctx->embedded_count == 1 ? table :
            nir_load_global_constant(b, 1, 64,
               nir_iadd_imm(b, table, tex->sampler_index * sizeof(uint64_t)), .align_mul = 8);`));
  v('kk_shader.c', s => s
    .replace('#include "kk_sampler.h"', '#include "kk_sampler.h"\n#include "kk_embedded_sampler_runtime.h"')
    .replace('   mtl_release(shader->dgc_compute);',
      '   kk_shader_embedded_samplers_finish(dev, shader);\n   mtl_release(shader->dgc_compute);')
    .replace(`      uint64_t *embedded = info->embedded_sampler_count ? calloc(info->embedded_sampler_count, sizeof(uint64_t)) : NULL;
      if (info->embedded_sampler_count && !embedded)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      for (uint32_t e = 0; e < info->embedded_sampler_count; ++e) {
         result = kk_heap_sampler_state(dev, &info->embedded_samplers[e], &embedded[e]);
         if (result != VK_SUCCESS) { free(embedded); return result; }
      }
      kk_lower_nir(dev, nir, emulated_stage, info->robustness,
                   info->set_layout_count, info->set_layouts, state, features, info, embedded);
      free(embedded);`,
      `      kk_lower_nir(dev, nir, emulated_stage, info->robustness,
                   info->set_layout_count, info->set_layouts, state, features, info, NULL);`)
    .replace(`                           state, &compile_options, pAllocator, &shaders[i]);

      if (result != VK_SUCCESS) {`,
      `                           state, &compile_options, pAllocator, &shaders[i]);
      if (result == VK_SUCCESS) {
         for (uint32_t input = 0; input < shader_count; ++input) {
            if (infos[input].stage != shaders[i]->vk.stage) continue;
            result = kk_shader_embedded_samplers_init(dev, shaders[i],
               infos[input].embedded_sampler_count, infos[input].embedded_samplers);
            break;
         }
         if (result != VK_SUCCESS) {
            kk_shader_destroy(&dev->vk, shaders[i], pAllocator);
            shaders[i] = NULL;
         }
      }

      if (result != VK_SUCCESS) {`)
    .replace('   desc->descriptor_heap = heap;\n   desc->push_size = push_size;',
      `   desc->descriptor_heap = heap;
   if (heap) {
      desc->root_dirty |= desc->heap_embedded_samplers[MESA_SHADER_COMPUTE] !=
                           shader->embedded_sampler_table;
      desc->heap_embedded_samplers[MESA_SHADER_COMPUTE] = shader->embedded_sampler_table;
   }
   desc->push_size = push_size;`)
    .replace('   cmd->state.shaders[stage] = shader;\n   cmd->state.dirty_shaders |= BITFIELD_BIT(stage);',
      `   if (shader && shader->info.descriptor_heap) {
      cmd->state.gfx.descriptors.root_dirty |=
         cmd->state.gfx.descriptors.heap_embedded_samplers[stage] != shader->embedded_sampler_table;
      cmd->state.gfx.descriptors.heap_embedded_samplers[stage] = shader->embedded_sampler_table;
   }
   cmd->state.shaders[stage] = shader;
   cmd->state.dirty_shaders |= BITFIELD_BIT(stage);`));
}
