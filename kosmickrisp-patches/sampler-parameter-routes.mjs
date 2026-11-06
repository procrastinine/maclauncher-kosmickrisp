// SPDX-License-Identifier: MIT
export const id = 'sampler-parameter-routes';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_sampler.c', s => s.replace(
    '   (void)params_lock_context;\n   *pSampler = kk_sampler_to_handle(sampler);',
    `   result = kk_sampler_cap_finalize(dev, sampler, pCreateInfo,
                                    params_lock_context);
   /* Native samplers retain logical identity independently of physical RCs.
    * Finalization has released every native sampler mutex before allocation. */
   if (result == VK_SUCCESS && sampler->params_gpu_address == 0) {
      struct kk_sampler_params params;
      kk_sampler_params_init(&params, pCreateInfo);
      result = params_lock_context == KK_SAMPLER_PARAMS_HEAP_LOCK_HELD
         ? kk_sampler_params_alloc_locked(dev, &params,
                                           &sampler->params_gpu_address)
         : kk_sampler_params_alloc(dev, &params, &sampler->params_gpu_address);
   }
   if (result != VK_SUCCESS) {
      kk_DestroySampler(device, kk_sampler_to_handle(sampler), pAllocator);
      return result;
   }
   *pSampler = kk_sampler_to_handle(sampler);`));

  edit('src/kosmickrisp/vulkan/kk_descriptor_set.c', s => s.replace(
    `         memcpy(desc[0].border, sampler->custom_border.uint32,
                sizeof(sampler->custom_border));
      }
   }

   assert(sizeof(desc[0]) * plane_count <= dst_size);`,
    `         memcpy(desc[0].border, sampler->custom_border.uint32,
                sizeof(sampler->custom_border));
      }

      assert(sampler->params_gpu_address != 0);
      for (uint8_t plane = 0; plane < plane_count; plane++) {
         if (sampler->is_virtual) {
            const uint8_t sampler_plane =
               MIN2(plane, sampler->plane_count - 1u);
            desc[plane].sampler_filter_bits =
               (sampler->filter_bits & ~3u) |
               (sampler->plane_filter_bits[sampler_plane] & 3u) |
               KK_SAMPLER_FILTER_VIRTUAL;
         }
         kk_sampler_params_pack_gpu_address(&desc[plane],
                                            sampler->params_gpu_address);
      }
   }

   assert(sizeof(desc[0]) * plane_count <= dst_size);`));
}
