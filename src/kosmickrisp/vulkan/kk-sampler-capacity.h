/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_CAPACITY_LOGICAL_H
#define KK_SAMPLER_CAPACITY_LOGICAL_H
#include "kk-sampler-cap-keys.h"
#include "kk-sampler-params-init.h"
#include "kk_sampler_params_api.h"

static VkResult
kk_sampler_cap_add_plane(struct kk_device *dev, struct kk_sampler *sampler,
                         unsigned plane, struct mtl_sampler_packed packed,
                         bool native_only)
{
   if (sampler->is_virtual)
      return kk_sampler_cap_get_canonical(dev, &packed,
         &sampler->planes[plane].hw, native_only);
   VkResult result = (native_only ? kk_sampler_native_add : kk_sampler_heap_add)(
      dev, packed, &sampler->planes[plane].hw);
   if (result == VK_SUCCESS &&
       !kk_sampler_cap_keys_equal(&packed, &sampler->planes[plane].hw->key))
      sampler->is_virtual = true;
   return result;
}

VkResult
kk_sampler_cap_finalize(struct kk_device *dev, struct kk_sampler *sampler,
                         const VkSamplerCreateInfo *info,
                         enum kk_sampler_params_lock_context lock_context)
{
   if (sampler->is_virtual) {
      /* Canonical taps always use weighted filtering. Preserve the original
       * logical reduction mode in its existing descriptor field on every GPU. */
      sampler->reduction_mode = sampler->vk.reduction_mode;
      /* Both reconstruction planes must use unclamped nearest tap states. */
      for (unsigned plane = 0; plane < sampler->plane_count; ++plane) {
         struct kk_rc_sampler *original = sampler->planes[plane].hw;
         if (original->canonical)
            continue;
         struct kk_rc_sampler *canonical;
         VkResult result = kk_sampler_cap_get_canonical(dev, &original->key,
            &canonical, sampler->native_only);
         if (result != VK_SUCCESS)
            return result;
         sampler->planes[plane].hw = canonical;
         kk_sampler_heap_remove(dev, original);
      }
      for (unsigned plane = 0; plane < 2; ++plane) {
         uint16_t filters = sampler->filter_bits & 3u;
         if (plane && sampler->vk.ycbcr_conversion && !sampler->has_border) {
            VkFilter chroma = sampler->vk.ycbcr_conversion->state.chroma_filter;
            filters = chroma == VK_FILTER_LINEAR ? 3u : 0u;
         }
         sampler->plane_filter_bits[plane] = (sampler->filter_bits & ~3u) | filters;
      }
   }
   struct kk_sampler_params params;
   kk_sampler_params_init(&params, info, sampler->vk.reduction_mode);
   /* Native materialization above has released its mutex. A heap producer
    * carries its existing lock explicitly; ordinary creation takes it once. */
   if (lock_context == KK_SAMPLER_PARAMS_HEAP_LOCK_HELD)
      return kk_sampler_params_alloc_locked(dev, &params, &sampler->params_gpu_address);
   assert(lock_context == KK_SAMPLER_PARAMS_UNLOCKED);
   return kk_sampler_params_alloc(dev, &params, &sampler->params_gpu_address);
}
#endif
