/* Copyright 2026 MacLauncher contributors. SPDX-License-Identifier: MIT */
#ifndef KK_DEVICE_FAULT_H
#define KK_DEVICE_FAULT_H

#include <stdbool.h>
#include <stdint.h>
#include "c11/threads.h"
#include "vulkan/vulkan_core.h"

struct kk_device;
struct mtl_feedback_data;

/* Storage exists only when deviceFault is enabled. Public advertisement is
 * intentionally separate from this source-only reporting implementation. */
#define KK_DEVICE_FAULT_CAPACITY 16
struct kk_device_fault {
   mtx_t mutex;
   cnd_t changed;
   uint64_t next_group;
   uint32_t head, count;
   bool have_first;
   VkDeviceFaultInfoKHR first;
   VkDeviceFaultInfoKHR pending[KK_DEVICE_FAULT_CAPACITY];
};

bool kk_fault_reports_init(struct kk_device_fault *fault);
void kk_fault_reports_finish(struct kk_device_fault *fault);
void kk_fault_reports_record(struct kk_device_fault *fault, uint64_t code,
                             const char *message, bool watchdog_timeout);
VkResult kk_fault_reports_get(struct kk_device_fault *fault, uint64_t timeout,
                              uint32_t *count, VkDeviceFaultInfoKHR *reports);
VkResult kk_fault_reports_get_ext(struct kk_device_fault *fault,
                                  VkDeviceFaultCountsEXT *counts,
                                  VkDeviceFaultInfoEXT *info);
VkResult kk_device_fault_init(struct kk_device *dev);
void kk_device_fault_finish(struct kk_device *dev);
void kk_device_fault_record(struct kk_device *dev,
                            const struct mtl_feedback_data *feedback);

#endif
