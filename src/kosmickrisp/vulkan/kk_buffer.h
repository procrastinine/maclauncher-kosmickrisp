/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_BUFFER_H
#define KK_BUFFER_H 1

#include "kk_device_memory.h"
#include "kk_private.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_buffer.h"
#include "kk_descriptor_types.h"

struct kk_buffer {
   struct vk_buffer vk;
   mtl_buffer *sparse_residency;
   uint32_t sparse_record;
   struct kk_buffer *heap_next;
   bool heap_registered;

   struct {
      mtl_buffer *handle;
      /* Offset within handle's base address. Don't use this directly, route all
       * offset calculations through kk_buffer_mtl_offset */
      uint64_t offset;
   } metal;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_buffer, vk.base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)

static inline struct kk_addr_range
kk_buffer_addr_range(const struct kk_buffer *buffer, uint64_t offset,
                     uint64_t range)
{
   if (buffer == NULL)
      return (struct kk_addr_range){.range = 0};

   return (struct kk_addr_range){
      .addr = vk_buffer_address(&buffer->vk, offset),
      .range = vk_buffer_range(&buffer->vk, offset, range),
   };
}

/* One bounded-buffer representation serves sets, dynamic sets and heaps.
 * The final word is reserved for the shared residency record; zero denotes
 * an ordinary or null buffer. */
static inline struct kk_buffer_address
kk_buffer_descriptor(const struct kk_buffer *buffer, struct kk_addr_range addr_range)
{
   (void)buffer;
   assert(addr_range.range <= UINT32_MAX);
   return (struct kk_buffer_address){
      .base_addr = addr_range.addr,
      .size = addr_range.range,
      .zero = buffer ? buffer->sparse_record : 0,
   };
}

static inline uint64_t
kk_buffer_mtl_offset(const struct kk_buffer *buffer, uint64_t offset)
{
   return buffer->metal.offset + offset;
}

static inline mtl_resource *
kk_buffer_to_mtl_resource(const struct kk_buffer *buffer)
{
   if (buffer != NULL) {
      return (mtl_resource *)buffer->metal.handle;
   }
   return NULL;
}

void kk_buffer_set_label(struct kk_buffer *buf, const char *label);

#endif // KK_BUFFER_H
