/* Copyright 2026 MacLauncher contributors. SPDX-License-Identifier: MIT */
#include "kk_device_fault.h"
#include "kk_device.h"
#include "kk_entrypoints.h"
#include "kosmickrisp/bridge/mtl_types.h"
#include "vk_alloc.h"

VkResult
kk_device_fault_init(struct kk_device *dev)
{
   if (!dev->vk.enabled_features.deviceFault)
      return VK_SUCCESS;
   struct kk_device_fault *fault = vk_zalloc(&dev->vk.alloc, sizeof(*fault), 8,
                                             VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!fault)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!kk_fault_reports_init(fault)) {
      vk_free(&dev->vk.alloc, fault);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   dev->fault = fault;
   return VK_SUCCESS;
}

void
kk_device_fault_finish(struct kk_device *dev)
{
   if (dev->fault) {
      kk_fault_reports_finish(dev->fault);
      vk_free(&dev->vk.alloc, dev->fault);
   }
}

void
kk_device_fault_record(struct kk_device *dev,
                        const struct mtl_feedback_data *feedback)
{
   /* Called only within the existing native-error arm. Successful submit and
    * completion paths perform no new check, allocation, lock or string copy. */
   if (dev->fault)
      kk_fault_reports_record(dev->fault, (uint32_t)feedback->error,
                              feedback->error_message,
                              feedback->error == MTL_COMMAND_QUEUE_ERROR_TIMEOUT);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetDeviceFaultReportsKHR(VkDevice device, uint64_t timeout,
                            uint32_t *pFaultCounts, VkDeviceFaultInfoKHR *pFaultInfo)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   return kk_fault_reports_get(dev->fault, timeout, pFaultCounts, pFaultInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetDeviceFaultDebugInfoKHR(VkDevice device, VkDeviceFaultDebugInfoKHR *pDebugInfo)
{
   /* Neither a vendor binary nor shader-abort data is captured. */
   (void)device;
   pDebugInfo->vendorBinarySize = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetDeviceFaultInfoEXT(VkDevice device, VkDeviceFaultCountsEXT *pFaultCounts,
                        VkDeviceFaultInfoEXT *pFaultInfo)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   if (!dev->fault) {
      pFaultCounts->addressInfoCount = 0;
      pFaultCounts->vendorInfoCount = 0;
      pFaultCounts->vendorBinarySize = 0;
      if (pFaultInfo)
         pFaultInfo->description[0] = '\0';
      return VK_SUCCESS;
   }
   return kk_fault_reports_get_ext(dev->fault, pFaultCounts, pFaultInfo);
}
