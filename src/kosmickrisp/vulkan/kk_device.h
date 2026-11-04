/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_DEVICE_H
#define KK_DEVICE_H 1

#include "kk_private.h"

#include "kk_query_table.h"
#include "kk_queue.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "kosmickrisp/clc/kk_precompiled_shader.h"
#include "libkk_shaders.h"

#include "util/u_dynarray.h"
#include "util/cnd_monotonic.h"

#include "vk_device.h"
#include "vk_meta.h"
#include "vk_queue.h"
#include "util/u_call_once.h"

struct kk_heap_sampler;
struct kk_heap_metadata_page;
struct kk_heap_border_registry;
struct kk_buffer;
struct kk_bo;
struct kk_physical_device;
struct kk_frame_capture;
struct vk_pipeline_cache;
struct kk_perf;
struct kk_device_fault;

struct kk_residency_set {
   simple_mtx_t mutex;
   bool dirty; /* Protected by mutex, including commit. */
   mtl_residency_set *handle;
   struct list_head allocations;
   /* Requested only for the high-priority working set, including while idle. */
   mtl_residency_set *priority_handle;
};

struct mtl_sampler_packed {
   enum mtl_sampler_address_mode mode_u;
   enum mtl_sampler_address_mode mode_v;
   enum mtl_sampler_address_mode mode_w;
   enum mtl_sampler_border_color border_color;

   enum mtl_sampler_min_mag_filter min_filter;
   enum mtl_sampler_min_mag_filter mag_filter;
   enum mtl_sampler_mip_filter mip_filter;
   enum mtl_sampler_reduction_mode reduction_mode;

   enum mtl_compare_function compare_func;
   float lod_bias;
   float min_lod;
   float max_lod;
   uint32_t max_anisotropy;
   bool normalized_coordinates;
};

struct kk_rc_sampler {
   struct mtl_sampler_packed key;

   mtl_sampler *handle;

   /* Reference count for this hardware sampler, protected by the heap mutex */
   uint32_t refcount;
   bool canonical;

   /* Index of this hardware sampler in the hardware sampler heap */
   uint16_t index;
   bool sparse_query_source;
};

struct kk_sampler_native_cache;
struct kk_sampler_heap {
   struct kk_sampler_native_cache *native_cache;
   uint32_t canonical_count;
   uint32_t indexed_count;
   bool sparse_queries;
   simple_mtx_t lock;

   struct kk_query_table table;

   /* Map of mtl_sampler_packed to kk_rc_sampler */
   struct hash_table *ht;
};

struct kk_precompiled_cache {
   struct kk_precompiled_shader shaders[LIBKK_NUM_PROGRAMS];
   util_once_flag once[LIBKK_NUM_PROGRAMS];
   VkResult result[LIBKK_NUM_PROGRAMS];
};

struct kk_atomic64_state;

struct kk_device {
   struct vk_device vk;
   simple_mtx_t pipeline_cache_init_mutex;
   struct kk_perf *perf;
   /* Monotonic: published before the first emulating sampler handle. */
   uint32_t sampler_emulation;
   struct kk_device_fault *fault;
   simple_mtx_t depth_stencil_cache_mutex;
   struct hash_table *depth_stencil_cache;
   struct kk_sparse_device_state *sparse;
   uint32_t sparse_residency_used;
   struct kk_sparse_sample_row *sparse_sample_rows;
   simple_mtx_t descriptor_heap_lock;
   struct kk_heap_cache *heap_samplers;
   struct kk_heap_metadata_page *heap_metadata_pages;
   struct kk_heap_border_registry *heap_borders;
   struct kk_buffer *heap_buffers;

   struct kk_vrs_device *vrs;
   util_once_flag dgc_once;
   struct kk_dgc_metal *dgc;
   struct kk_temporal *temporal;
   mtl_device *mtl_handle;
   mtl_compiler *mtl_compiler_handle;
   /* Created on the first ray tracing pipeline; links visible functions. */
   mtl_compiler *mtl_rt_compiler;
   struct kk_async_compute_owner *async_compute;

   /* Dispatch table exposed to the user. Required since we need to record all
    * commands due to Metal limitations */
   struct vk_device_dispatch_table exposed_dispatch_table;

   /* Published only after successful lazy allocation, retired on free. */
   uint32_t live_lazy_allocations;
   bool poly_layer_argument;
   PFN_vkCmdBeginRendering whole_pass_begin_rendering;
   PFN_vkCmdEndRendering2KHR whole_pass_end_rendering;

   struct kk_sampler_heap samplers;
   struct kk_query_table occlusion_queries;

   /* Accepted native timestamp heap capacity, published after negotiation. */
   uint32_t timestamp_heap_capacity;

   /* Track all heaps the user allocated so we can set them all as resident when
    * recording as required by Metal. */
   struct kk_residency_set residency_set;

   struct kk_precompiled_cache precompiled_cache;

   uint32_t queue_count;
   /* Two queues in family 0 and one in each remaining family. */
   struct kk_queue queues[5];
   simple_mtx_t blit_queue_mutex;
   struct kk_queue *blit_queue;
   /* Legacy zero-fill queue, atomically published on first private flagged
    * allocation under residency_set.mutex; no WSI queue dependency.
    */
   mtl_command_queue *zero_fill_queue;

   struct vk_meta_device meta;

   /* Geomtry heap */
   struct kk_atomic64_state *atomic64_locks;
   struct kk_bo *heap;
   struct kk_bo *interlock_guard;
   struct kk_interlock_registry *interlock_registry;
   util_once_flag heap_init_once;
   struct kk_frame_capture *frame_capture;

   /* Host publication waits only; independent of native signal completion
    * and the per-sync publisher lifetime mutex. */
   mtx_t pending_mutex;
   struct u_cnd_monotonic pending_cond;
};

VK_DEFINE_HANDLE_CASTS(kk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

static inline struct kk_physical_device *
kk_device_physical(const struct kk_device *dev)
{
   return (struct kk_physical_device *)dev->vk.physical;
}

VkResult kk_device_init_meta(struct kk_device *dev);
void kk_device_finish_meta(struct kk_device *dev);
VkResult kk_build_precompiled_shader(struct kk_device *dev, const uint32_t *binary, uint32_t max_threads, struct kk_precompiled_shader *out);
VkResult kk_device_init_lib(struct kk_device *dev);
VkResult kk_device_init_conservative_lib(struct kk_device *dev);
VkResult kk_device_init_primitives_generated_lib(struct kk_device *dev);
VkResult kk_device_ensure_precompiled_shader(struct kk_device *dev,
                                          enum libkk_program idx);
struct kk_precompiled_shader *kk_device_get_precompiled(struct kk_device *dev,
                                                       enum libkk_program idx);
void kk_device_finish_lib(struct kk_device *dev);
void kk_depth_stencil_cache_finish(struct kk_device *dev);

void kk_device_add_bo_to_residency_set(struct kk_device *dev, struct kk_bo *bo);
void kk_device_add_imported_bo_to_residency_set(struct kk_device *dev,
                                               struct kk_bo *bo);
void kk_device_remove_bo_from_residency_set(struct kk_device *dev, struct kk_bo *bo);
void kk_device_add_heap_to_residency_set(struct kk_device *dev, mtl_heap *heap);
void kk_device_remove_heap_from_residency_set(struct kk_device *dev,
                                              mtl_heap *heap);
void kk_device_add_buffer_to_residency_set(struct kk_device *dev,
                                           mtl_buffer *buffer);
void kk_device_remove_buffer_from_residency_set(struct kk_device *dev,
                                                mtl_buffer *buffer);
void kk_device_make_resources_resident(struct kk_device *dev);

/* Required to create a sampler */
mtl_sampler *kk_sampler_create(struct kk_device *dev,
                               const struct mtl_sampler_packed *packed);
/* Requires dev->samplers.lock; never acquires the descriptor heap lock. */
VkResult kk_sampler_cap_reserve_locked(struct kk_device *dev);
VkResult kk_sampler_heap_add(struct kk_device *dev,
                             struct mtl_sampler_packed desc,
                             struct kk_rc_sampler **out);
VkResult kk_sampler_native_add(struct kk_device *dev,
   struct mtl_sampler_packed desc, struct kk_rc_sampler **out);
void kk_sampler_heap_remove(struct kk_device *dev, struct kk_rc_sampler *rc);

#endif // KK_DEVICE_H
