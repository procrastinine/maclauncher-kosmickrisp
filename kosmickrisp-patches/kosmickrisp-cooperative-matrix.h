/* SPDX-License-Identifier: MIT */
#pragma once

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetPhysicalDeviceCooperativeMatrixPropertiesKHR(
   VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount,
   VkCooperativeMatrixPropertiesKHR *pProperties)
{
   VK_FROM_HANDLE(kk_physical_device, pdev, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkCooperativeMatrixPropertiesKHR, out, pProperties, pPropertyCount);
   if (pdev->info.gpu_apple_family < 7)
      return vk_outarray_status(&out);
   static const struct {
      VkComponentTypeKHR a, b, c, result;
   } types[] = {
      { VK_COMPONENT_TYPE_FLOAT16_KHR, VK_COMPONENT_TYPE_FLOAT16_KHR,
        VK_COMPONENT_TYPE_FLOAT16_KHR, VK_COMPONENT_TYPE_FLOAT16_KHR },
      { VK_COMPONENT_TYPE_FLOAT16_KHR, VK_COMPONENT_TYPE_FLOAT16_KHR,
        VK_COMPONENT_TYPE_FLOAT32_KHR, VK_COMPONENT_TYPE_FLOAT32_KHR },
      { VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT, VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT,
        VK_COMPONENT_TYPE_FLOAT32_KHR, VK_COMPONENT_TYPE_FLOAT32_KHR },
      { VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT, VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT,
        VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT, VK_COMPONENT_TYPE_FLOAT32_KHR },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
      vk_outarray_append_typed(VkCooperativeMatrixPropertiesKHR, &out, p) {
         p->MSize = p->NSize = p->KSize = 16;
         p->AType = types[i].a;
         p->BType = types[i].b;
         p->CType = types[i].c;
         p->ResultType = types[i].result;
         p->saturatingAccumulation = VK_FALSE;
         p->scope = VK_SCOPE_SUBGROUP_KHR;
      }
   }
   return vk_outarray_status(&out);
}
