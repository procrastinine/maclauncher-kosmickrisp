/* Copyright 2026 MacLauncher contributors. SPDX-License-Identifier: MIT */
#include "kk_device_fault.h"

#include <string.h>
#include <time.h>

/* NSString supplies valid UTF-8. Do not cut its last codepoint when bounding
 * the retained Vulkan description. No native string/object escapes callback. */
static void
copy_description(char dst[VK_MAX_DESCRIPTION_SIZE], const char *src)
{
   if (!src)
      src = "Metal command queue reported an error";
   size_t length = strlen(src);
   if (length >= VK_MAX_DESCRIPTION_SIZE) {
      length = VK_MAX_DESCRIPTION_SIZE - 1;
      while (length && ((unsigned char)src[length] & 0xc0) == 0x80)
         length--;
   }
   memcpy(dst, src, length);
   dst[length] = '\0';
}

bool
kk_fault_reports_init(struct kk_device_fault *fault)
{
   memset(fault, 0, sizeof(*fault));
   if (mtx_init(&fault->mutex, mtx_plain) != thrd_success)
      return false;
   if (cnd_init(&fault->changed) != thrd_success) {
      mtx_destroy(&fault->mutex);
      return false;
   }
   return true;
}

void
kk_fault_reports_finish(struct kk_device_fault *fault)
{
   cnd_destroy(&fault->changed);
   mtx_destroy(&fault->mutex);
}

void
kk_fault_reports_record(struct kk_device_fault *fault, uint64_t code,
                        const char *message, bool watchdog_timeout)
{
   VkDeviceFaultInfoKHR report = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR,
      .flags = VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR |
               VK_DEVICE_FAULT_FLAG_VENDOR_KHR,
      /* Metal's public commit feedback does not provide a GPU fault address
       * or instruction address. Leave their flags and zeroed fields alone. */
      .vendorInfo.vendorFaultCode = code,
   };
   if (watchdog_timeout)
      report.flags |= VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR;
   copy_description(report.description, message);
   copy_description(report.vendorInfo.description, message);

   mtx_lock(&fault->mutex);
   report.groupId = ++fault->next_group;
   if (!fault->have_first) {
      fault->first = report;
      fault->have_first = true;
   }
   if (fault->count < KK_DEVICE_FAULT_CAPACITY) {
      unsigned index = (fault->head + fault->count) % KK_DEVICE_FAULT_CAPACITY;
      fault->pending[index] = report;
      fault->count++;
   } else {
      /* Preserve occurrence order and indicate the actual information loss.
       * The last retained entry owns the overflow indication. */
      unsigned index = (fault->head + fault->count - 1) % KK_DEVICE_FAULT_CAPACITY;
      fault->pending[index].flags |= VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR;
   }
   cnd_broadcast(&fault->changed);
   mtx_unlock(&fault->mutex);
}

VkResult
kk_fault_reports_get(struct kk_device_fault *fault, uint64_t timeout,
                     uint32_t *count, VkDeviceFaultInfoKHR *reports)
{
   if (!fault) {
      /* An enabled extension may be queried without deviceFault enabled.
       * Honor its timeout without allocating dormant report storage. */
      *count = 0;
      if (timeout) {
         struct timespec remaining = {
            .tv_sec = timeout / 1000000000,
            .tv_nsec = timeout % 1000000000,
         };
         int result;
         do {
            result = thrd_sleep(&remaining, &remaining);
         } while (result == -1);
         if (result != 0)
            return VK_ERROR_UNKNOWN;
      }
      return VK_TIMEOUT;
   }
   struct timespec deadline;
   if (timeout && timeout != UINT64_MAX) {
      timespec_get(&deadline, TIME_UTC);
      deadline.tv_sec += timeout / 1000000000;
      deadline.tv_nsec += timeout % 1000000000;
      if (deadline.tv_nsec >= 1000000000) {
         deadline.tv_sec++;
         deadline.tv_nsec -= 1000000000;
      }
   }
   mtx_lock(&fault->mutex);
   while (!fault->count) {
      if (!timeout) {
         *count = 0;
         mtx_unlock(&fault->mutex);
         return VK_TIMEOUT;
      }
      int result = timeout == UINT64_MAX
         ? cnd_wait(&fault->changed, &fault->mutex)
         : cnd_timedwait(&fault->changed, &fault->mutex, &deadline);
      if (result != thrd_success && !fault->count) {
         *count = 0;
         mtx_unlock(&fault->mutex);
         return result == thrd_timedout ? VK_TIMEOUT : VK_ERROR_UNKNOWN;
      }
   }
   if (!reports) {
      *count = fault->count; /* Count queries never consume reports. */
      mtx_unlock(&fault->mutex);
      return VK_SUCCESS;
   }
   uint32_t written = *count < fault->count ? *count : fault->count;
   for (uint32_t i = 0; i < written; i++)
      reports[i] = fault->pending[(fault->head + i) % KK_DEVICE_FAULT_CAPACITY];
   fault->head = (fault->head + written) % KK_DEVICE_FAULT_CAPACITY;
   fault->count -= written;
   *count = written;
   VkResult result = fault->count ? VK_INCOMPLETE : VK_SUCCESS;
   mtx_unlock(&fault->mutex);
   return result;
}

VkResult
kk_fault_reports_get_ext(struct kk_device_fault *fault,
                         VkDeviceFaultCountsEXT *counts,
                         VkDeviceFaultInfoEXT *info)
{
   mtx_lock(&fault->mutex);
   uint32_t available = fault->have_first ? 1 : 0;
   uint32_t capacity = info ? counts->vendorInfoCount : 0;
   counts->addressInfoCount = 0;
   counts->vendorInfoCount = available;
   counts->vendorBinarySize = 0;
   VkResult result = VK_SUCCESS;
   if (info) {
      if (available) {
         memcpy(info->description, fault->first.description,
                sizeof(info->description));
         if (capacity && info->pVendorInfos)
            info->pVendorInfos[0] = fault->first.vendorInfo;
         else
            result = VK_INCOMPLETE;
         counts->vendorInfoCount = capacity && info->pVendorInfos ? 1 : 0;
      } else {
         info->description[0] = '\0';
      }
   }
   mtx_unlock(&fault->mutex);
   return result;
}
