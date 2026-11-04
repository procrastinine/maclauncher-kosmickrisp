/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_MEMORY_H
#define KK_MEMORY_H 1

#include "kk_private.h"

#include "kk_bo.h"

#include "vk_device_memory.h"

#include "util/list.h"
#include "util/simple_mtx.h"

struct kk_device_memory {
   struct vk_device_memory vk;
   struct kk_bo *bo;
   struct kk_standard_3d_page *standard_3d;
   void *map;
   bool lazy;
   uint32_t image_bind_count;
   simple_mtx_t transient_mutex;
   float priority;
   bool priority_resident;
   struct list_head priority_link;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_device_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)

VkResult kk_device_memory_commit_private(struct kk_device *, struct kk_device_memory *);

/* Logical VkDeviceMemory bytes to native heap placement bytes. */
static inline uint64_t
kk_device_memory_heap_offset_B(const struct kk_device_memory *mem,
                                uint64_t logical_offset_B)
{
   return mem->bo->heap_offset_B + logical_offset_B;
}

extern const VkExternalMemoryProperties kk_mtlheap_mem_props;
extern const VkExternalMemoryProperties kk_host_mem_props;
extern const VkExternalMemoryProperties kk_fd_mem_props;

void kk_device_memory_set_label(struct kk_device_memory *dev_mem,
                                const char *label);

#endif // KK_MEMORY_H
