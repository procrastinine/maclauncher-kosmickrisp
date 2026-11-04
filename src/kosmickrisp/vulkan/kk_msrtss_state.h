/* SPDX-License-Identifier: MIT */
#ifndef KK_MSRTSS_STATE_H
#define KK_MSRTSS_STATE_H
#include "vk_util.h"

static inline VkSampleCountFlagBits
kk_msrtss_samples(const VkRenderingInfo *info)
{
   const VkMultisampledRenderToSingleSampledInfoEXT *ms =
      vk_find_struct_const(info->pNext,
                           MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT);
   return ms && ms->multisampledRenderToSingleSampledEnable
      ? ms->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT;
}
#endif
