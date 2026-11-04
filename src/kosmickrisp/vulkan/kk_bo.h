/*
 * Copyright © 2025 LunarG, Inc
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_BO_H
#define KK_BO_H 1

#include "kosmickrisp/bridge/mtl_types.h"
#include "util/list.h"

#include "vulkan/vulkan_core.h"

#include <inttypes.h>
#include <stdbool.h>

struct kk_device;
struct vk_object_base;

struct kk_bo {
   mtl_heap *mtl_handle;
   mtl_buffer *map;
   uint64_t size_B;
   uint64_t heap_offset_B;
   uint64_t gpu; // GPU address
   void *cpu;    // CPU address
   struct list_head residency_link;
   struct list_head residency_alias_link;
   bool export_heap_region; /* Publish immutable bounds only on handle export. */
   bool priority_resident; /* Protected by the residency-set mutex. */
};

struct kk_ptr {
   void *cpu;
   uint64_t gpu;

   /* Pointer in terms of a Metal buffer and offset */
   mtl_buffer *buffer;
   uint32_t offset;
};

VkResult kk_alloc_bo(struct kk_device *dev, struct vk_object_base *log_obj,
                     uint64_t size_B, uint64_t align_B, struct kk_bo **bo_out);

VkResult kk_alloc_bo_options(struct kk_device *dev, struct vk_object_base *log_obj,
                             uint64_t size_B, uint64_t align_B,
                             enum mtl_resource_options options,
                             struct kk_bo **bo_out);

void kk_destroy_bo(struct kk_device *dev, struct kk_bo *bo);

VkResult kk_bo_map_placed(struct kk_device *dev, struct kk_bo *bo, void **addr);
VkResult kk_bo_unmap(struct kk_device *dev, struct kk_bo *bo, void *addr,
                     bool reserved);

void kk_bo_set_label(struct kk_bo *bo, const char *label);

#endif /* KK_BO_H */
