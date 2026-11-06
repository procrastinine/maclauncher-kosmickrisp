// SPDX-License-Identifier: MIT
// Native heap samplers share sampler construction/cache with ordinary sets.
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_device.h', s => s
    .replace('   uint16_t refcount;', '   uint32_t refcount;')
    .replace('struct kk_sampler_heap {', 'struct kk_sampler_heap {\n   uint32_t indexed_count;')
    .replace('void kk_sampler_heap_remove(', `VkResult kk_sampler_native_add(struct kk_device *dev,
   struct mtl_sampler_packed desc, struct kk_rc_sampler **out);
void kk_sampler_heap_remove(`));
  edit('src/kosmickrisp/vulkan/kk_device.c', s => s
    .replace('static VkResult\nkk_sampler_heap_add_locked(', `static VkResult
kk_sampler_require_index(struct kk_device *dev, struct kk_sampler_heap *h,
                         struct kk_rc_sampler *rc)
{
   if (rc->index != UINT16_MAX)
      return VK_SUCCESS;
   const struct kk_physical_device *pdev = kk_device_physical(dev);
   if (h->indexed_count >= MIN2(pdev->info.max_sampler_count, MSL_MAX_SAMPLERS))
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   uint32_t index;
   VkResult result = kk_query_table_add(dev, &h->table,
      mtl_sampler_get_gpu_resource_id(rc->handle), &index);
   if (result == VK_SUCCESS) {
      rc->index = index;
      ++h->indexed_count;
   }
   return result;
}

static VkResult
kk_sampler_heap_add_locked(`)
    .replace('                           struct kk_rc_sampler **out)\n{',
      '                           struct kk_rc_sampler **out, bool needs_index)\n{')
    .replace('      assert((*out)->refcount != 0);', `      if (needs_index) {
         VkResult result = kk_sampler_require_index(dev, h, *out);
         if (result != VK_SUCCESS)
            return result;
      }
      assert((*out)->refcount != 0);`)
    .replace(`   /* Constrain to device max sampler count */
   if (h->ht->entries >= pdev->info.max_sampler_count)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

`, '')
    .replace(`   uint64_t gpu_id = mtl_sampler_get_gpu_resource_id(handle);

   uint32_t index;
   VkResult result = kk_query_table_add(dev, &h->table, gpu_id, &index);
   if (result != VK_SUCCESS) {
      mtl_release(handle);
      ralloc_free(rc);
      return result;
   }
`, `   if (!handle) {
      ralloc_free(rc);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
`)
    .replace('      .index = index,', '      .index = UINT16_MAX,')
    .replace('   _mesa_hash_table_insert(h->ht, &rc->key, rc);', `   if (needs_index) {
      VkResult result = kk_sampler_require_index(dev, h, rc);
      if (result != VK_SUCCESS) {
         mtl_release(handle);
         ralloc_free(rc);
         return result;
      }
   }
   _mesa_hash_table_insert(h->ht, &rc->key, rc);`)
    .replace('kk_sampler_heap_add_locked(dev, h, desc, out);',
      'kk_sampler_heap_add_locked(dev, h, desc, out, true);')
    .replace('static void\nkk_sampler_heap_remove_locked(', `VkResult
kk_sampler_native_add(struct kk_device *dev, struct mtl_sampler_packed desc,
                       struct kk_rc_sampler **out)
{
   struct kk_sampler_heap *h = &dev->samplers;
   simple_mtx_lock(&h->lock);
   VkResult result = kk_sampler_heap_add_locked(dev, h, desc, out, false);
   simple_mtx_unlock(&h->lock);
   return result;
}

static void
kk_sampler_heap_remove_locked(`)
    .replace('      kk_query_table_remove(dev, &h->table, rc->index);', `      if (rc->index != UINT16_MAX) {
         kk_query_table_remove(dev, &h->table, rc->index);
         --h->indexed_count;
      }`));
  edit('src/kosmickrisp/vulkan/kk_sampler.h', s => s
    .replace('   bool has_border;', '   bool has_border;\n   bool native_only;')
    .replace('#endif /* KK_SAMPLER_H */', `VkResult kk_create_heap_sampler(struct kk_device *dev,
   const VkSamplerCreateInfo *info, VkSampler *sampler);

#endif /* KK_SAMPLER_H */`));
  edit('src/kosmickrisp/vulkan/kk_sampler.c', s => s
    .replace(`VKAPI_ATTR VkResult VKAPI_CALL
kk_CreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)`,
      `static VkResult
kk_create_sampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkSampler *pSampler,
                  bool native_only)`)
    .replace(`   bool custom_border =`, `   sampler->native_only = native_only;
   bool custom_border =`)
    .replaceAll('kk_sampler_heap_add(dev,',
      '(native_only ? kk_sampler_native_add : kk_sampler_heap_add)(dev,')
    .replace('VKAPI_ATTR void VKAPI_CALL\nkk_DestroySampler(', `VKAPI_ATTR VkResult VKAPI_CALL
kk_CreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
   return kk_create_sampler(device, pCreateInfo, pAllocator, pSampler, false);
}

VkResult
kk_create_heap_sampler(struct kk_device *dev, const VkSamplerCreateInfo *info,
                        VkSampler *sampler)
{
   return kk_create_sampler(kk_device_to_handle(dev), info, NULL, sampler, true);
}

VKAPI_ATTR void VKAPI_CALL
kk_DestroySampler(`));
  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', s => s.replace(
    `         desc[0].clamp_0_sampler_index_or_negative =
            sampler->planes[1].hw->index;`,
    `         desc[0].clamp_0_sampler_index_or_negative =
            sampler->native_only ? 0 : sampler->planes[1].hw->index;`));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace(
    `      nir_def *index = load_resource_deref_desc(
         b, 1, 16, nir_src_as_deref(nir_src_for_ssa(sampler)),
         plane_offset_B + offs, ctx);

      nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle,
                            nir_load_sampler_handle_kk(b, index));`,
    `      nir_deref_instr *sampler_deref =
         nir_src_as_deref(nir_src_for_ssa(sampler));
      nir_def *index;
      if (sampler_deref->deref_type == nir_deref_type_cast) {
         offs = clamp_to_0 ? offsetof(struct kk_sampled_image_descriptor, aux_word)
                          : offsetof(struct kk_sampled_image_descriptor, aux_resource_id);
         index = load_resource_addr(b, 1, 64, sampler_deref,
                                    plane_offset_B + offs, ctx);
      } else {
         index = load_resource_deref_desc(b, 1, 16, sampler_deref,
                                          plane_offset_B + offs, ctx);
      }

      nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle,
                            nir_load_sampler_handle_kk(b, index));`));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s.replace(
    `   case nir_intrinsic_load_sampler_handle_kk:
      P(ctx, "sampler_table.handles[");
      src_to_msl(ctx, &instr->src[0]);
      P(ctx, "];\\n");
      break;`,
    `   case nir_intrinsic_load_sampler_handle_kk:
      if (instr->src[0].ssa->bit_size == 64) {
         P(ctx, "*(constant sampler*)");
         src_to_msl(ctx, &instr->src[0]);
         P(ctx, ";\\n");
      } else {
         P(ctx, "sampler_table.handles[");
         src_to_msl(ctx, &instr->src[0]);
         P(ctx, "];\\n");
      }
      break;`));
  edit('src/kosmickrisp/compiler/msl_type_inference.c', s => s.replace(
    `   case nir_intrinsic_load_sampler_handle_kk:
      set_type(types, &instr->def, TYPE_SAMPLER);
      break;`,
    `   case nir_intrinsic_load_sampler_handle_kk:
      set_type(types, &instr->def, TYPE_SAMPLER);
      set_type(types, &instr->src[0], TYPE_UINT);
      break;`));
}
