/* SPDX-License-Identifier: MIT */
#include "kk_bo.h"
#include "kk_depth_bounds.h"
#include <string.h>

static VkResult
kk_interlock_device_init(struct kk_device *dev)
{
   if (!dev->vk.enabled_features.fragmentShaderSampleInterlock &&
       !dev->vk.enabled_features.fragmentShaderPixelInterlock &&
       !dev->vk.enabled_features.fragmentShaderShadingRateInterlock &&
       !kk_depth_bounds_emulated(dev) && !kk_mesh_depth_bias_device(dev))
      return VK_SUCCESS;
   if (kk_depth_bounds_emulated(dev) || kk_mesh_depth_bias_device(dev))
      kk_depth_bounds_init_dispatch(dev);
   /* One tile of pixel/sample guard words avoids a single contended address.
    * Atomic values are internal; aliased tiles never communicate app data. */
   VkResult result = kk_alloc_bo(dev, &dev->vk.base, 32 * 32 * 32 * sizeof(uint32_t),
                                 sizeof(uint32_t), &dev->interlock_guard);
   if (result == VK_SUCCESS) {
      memset(dev->interlock_guard->cpu, 0, dev->interlock_guard->size_B);
      kk_bo_set_label(dev->interlock_guard, "Fragment interlock guard");
   }
   return result;
}

static void
kk_interlock_device_finish(struct kk_device *dev)
{
   if (dev->interlock_guard)
      kk_destroy_bo(dev, dev->interlock_guard);
}
