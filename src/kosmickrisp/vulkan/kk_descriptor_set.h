/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_DESCRIPTOR_SET
#define KK_DESCRIPTOR_SET 1

#include "kk_private.h"

#include "kk_descriptor_types.h"
#include "kk_device.h"

#include "vk_descriptor_update_template.h"
#include "vk_object.h"

#include "util/list.h"
#include "util/vma.h"

struct kk_descriptor_set_layout;
struct kk_bo;

struct kk_descriptor_pool {
   struct vk_object_base base;

   struct list_head sets;

   struct kk_bo *bo;
   struct util_vma_heap heap;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)

void kk_descriptor_pool_set_label(struct kk_descriptor_pool *pool,
                                  const char *label);

struct kk_descriptor_set {
   struct vk_object_base base;

   /* Link in kk_descriptor_pool::sets */
   struct list_head link;

   struct kk_descriptor_set_layout *layout;
   void *mapped_ptr;
   uint64_t addr;
   uint32_t size;

   struct kk_buffer_address dynamic_buffers[];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)

static inline struct kk_buffer_address
kk_descriptor_set_addr(const struct kk_descriptor_set *set)
{
   return (struct kk_buffer_address){
      .base_addr = set->addr,
      .size = set->size,
   };
}

struct kk_push_descriptor_set {
   uint32_t capacity;
   struct kk_descriptor_set_layout *layout;

   /* Immutable upload owned by this command buffer, retired with this set. */
   const void *uploaded_data;
   uint64_t uploaded_addr;
   uint32_t uploaded_size;
   uint8_t data[];
};

void kk_push_descriptor_set_update(struct kk_push_descriptor_set *push_set,
                                   uint32_t write_count,
                                   const VkWriteDescriptorSet *writes);

void kk_push_descriptor_set_update_template(
   struct kk_push_descriptor_set *push_set,
   struct kk_descriptor_set_layout *layout,
   const struct vk_descriptor_update_template *template, const void *data);

struct vk_sampler_state;
struct kk_image;
struct kk_buffer;
void kk_descriptor_heap_init(struct kk_device *dev);
void kk_descriptor_heap_finish(struct kk_device *dev);
void kk_descriptor_heap_image_finish(struct kk_device *dev, struct kk_image *image);
void kk_descriptor_heap_buffer_bind(struct kk_device *dev, struct kk_buffer *buffer);
void kk_descriptor_heap_buffer_finish(struct kk_device *dev, struct kk_buffer *buffer);
/* Shared address registry and typed writer. Callers hold descriptor_heap_lock.
 * Registrations have the logical VkBuffer's lifetime. Address-only typed
 * writes create no native view/cache object. Lookup
 * returns a live covering registration, not an original-handle provenance
 * guarantee when logical address ranges overlap. */
void kk_descriptor_buffer_register_locked(struct kk_device *dev,
   struct kk_buffer *buffer);
struct kk_buffer *kk_descriptor_buffer_from_address_locked(struct kk_device *dev,
   uint64_t address, uint64_t size);
VkResult kk_descriptor_write_texel_buffer_locked(struct kk_device *dev,
   uint64_t address, uint64_t size, VkFormat format, VkDescriptorType type,
   void *destination, uint32_t descriptor_size);
/* No allocation, lookup or native realization. A failed validation returns
 * before touching destination. A valid supported selected source is encoded
 * infallibly; these are validation errors, never HOST/DEVICE OOM. The caller
 * holds descriptor_heap_lock and the selected source's valid host lifetime. */
VkResult kk_descriptor_write_raw_texel_buffer_locked(struct kk_device *dev,
   const struct kk_buffer *selected, uint64_t address, uint64_t size,
   VkFormat format, VkDescriptorType type, void *destination,
   uint32_t descriptor_size);
/* The same live source, or ordinary aliases with proven native intervals.
 * Different Sparse virtual sources and unknown storage are not equivalent.
 * This query acquires no resource ownership. */
bool kk_descriptor_raw_texel_sources_equal_locked(const struct kk_buffer *a,
   const struct kk_buffer *b, uint64_t address, uint64_t size, VkFormat format);

VkResult kk_heap_embedded_sampler_table(struct kk_device *dev,
   uint32_t count, const struct vk_sampler_state *states, uint64_t *gpu_address);
VkResult kk_heap_sampler_state(struct kk_device *dev,
   const struct vk_sampler_state *state, uint64_t *gpu_address);

#endif
