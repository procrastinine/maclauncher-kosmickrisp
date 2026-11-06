/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_PARAMETERS_INIT_H
#define KK_SAMPLER_PARAMETERS_INIT_H
#include <string.h>
#include "vulkan/vulkan_core.h"
#include "kosmickrisp/compiler/kk-sampler-params.h"

static inline void
kk_sampler_params_init(struct kk_sampler_params *params,
                       const VkSamplerCreateInfo *info)
{
   memset(params, 0, sizeof(*params));
   memcpy(&params->min_lod, &info->minLod, sizeof(float));
   memcpy(&params->max_lod, &info->maxLod, sizeof(float));
   memcpy(&params->lod_bias, &info->mipLodBias, sizeof(float));
   memcpy(&params->max_anisotropy, &info->maxAnisotropy, sizeof(float));
   if (info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
      params->flags |= KK_SAMPLER_PARAMS_MIPMAP_LINEAR;
   if (info->anisotropyEnable)
      params->flags |= KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE;
   if (info->compareEnable)
      params->compare_op = info->compareOp;
   else
      params->compare_op = VK_COMPARE_OP_ALWAYS;
   params->reduction_mode = VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
   for (const VkBaseInStructure *ext = info->pNext; ext; ext = ext->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO) {
         params->reduction_mode =
            ((const VkSamplerReductionModeCreateInfo *)ext)->reductionMode;
         break;
      }
   }
}
#endif
