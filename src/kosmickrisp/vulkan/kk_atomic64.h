/* SPDX-License-Identifier: MIT */
#ifndef KK_ATOMIC64_H
#define KK_ATOMIC64_H
#include "vk_util.h"
#include "kosmickrisp/bridge/mtl_types.h"
struct kk_device;
struct msl_compile_data;
struct kk_atomic64_state;
void kk_atomic64_finish_device(struct kk_device *dev);
VkResult kk_atomic64_prepare(struct kk_device *dev);
/* Base descriptor retains every non-sampler constant for false/true variants. */
VkResult kk_msl_function_sampler_base(struct kk_device *dev,
   const struct msl_compile_data *data, mtl_library *library, const char *entry,
   mtl_function_descriptor **out);
VkResult kk_msl_function(struct kk_device *dev,
   const struct msl_compile_data *data, mtl_library *library,
   const char *entry, mtl_function_descriptor **out);
#endif
