/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_device.h"
#include "kk_perf.h"
#include "kk_frame_capture.h"
#include "kk_sampler.h"
#include "kk-sampler-cap-keys.h"
#include "kk_bo.h"
#include "kk_sparse.h"
#include "kk_descriptor_set.h"
#include "kk_dgc.h"

#include "kk_buffer.h"
#include "kk_buffer_view.h"
#include "kk_cmd_buffer.h"
#include "kk_entrypoints.h"
#include "kk_image.h"
#include "kk_image_view.h"
#include "kk_instance.h"
#include "kk_physical_device.h"
#include "kk_shader.h"
#include "kk_fragment_shading_rate_cache.h"
#include "kk_interlock_device.h"
#include "kk_interlock_submit.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_device.h"
#include "kosmickrisp/bridge/ns_process_info.h"
#include "kosmickrisp/compiler/nir_to_msl.h"

#include "kk_dispatch_cmd.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"

#include "vulkan/wsi/wsi_common.h"
#include "vk_pipeline_cache.h"

#include <time.h>

struct kk_mtl_compiler {
   mtl_compiler *handle;
   uint32_t refcount;
};

static struct hash_table compilers_ht;
static simple_mtx_t compilers_ht_lock;
static once_flag compilers_ht_once = ONCE_FLAG_INIT;

static void
kk_init_compiler_table()
{
   _mesa_pointer_hash_table_init(&compilers_ht, NULL);
   simple_mtx_init(&compilers_ht_lock, mtx_plain);
}

static mtl_compiler *
kk_acquire_compiler(struct kk_device *dev)
{
   /* KK_WORKAROUND_11 */
   struct kk_physical_device *pdev = kk_device_physical(dev);
   if (pdev->settings.disabled_workarounds & BITFIELD64_BIT(11)) {
      return mtl_new_compiler(dev->mtl_handle);
   }

   call_once(&compilers_ht_once, kk_init_compiler_table);
   simple_mtx_lock(&compilers_ht_lock);

   struct hash_entry *ent =
      _mesa_hash_table_search(&compilers_ht, dev->mtl_handle);

   struct kk_mtl_compiler *compiler;
   if (ent == NULL) {
      compiler = ralloc(NULL, struct kk_mtl_compiler);
      if (compiler == NULL) {
         simple_mtx_unlock(&compilers_ht_lock);
         return NULL;
      }

      compiler->handle = mtl_new_compiler(dev->mtl_handle);
      if (compiler->handle == NULL) {
         ralloc_free(compiler);
         simple_mtx_unlock(&compilers_ht_lock);
         return NULL;
      }

      compiler->refcount = 1;
      _mesa_hash_table_insert(&compilers_ht, dev->mtl_handle, compiler);
   } else {
      compiler = ent->data;
      compiler->refcount++;
   }

   simple_mtx_unlock(&compilers_ht_lock);
   return compiler->handle;
}

static void
kk_release_compiler(struct kk_device *dev)
{
   /* KK_WORKAROUND_11 */
   struct kk_physical_device *pdev = kk_device_physical(dev);
   if (pdev->settings.disabled_workarounds & BITFIELD64_BIT(11)) {
      mtl_release(dev->mtl_compiler_handle);
      return;
   }

   simple_mtx_lock(&compilers_ht_lock);

   struct hash_entry *ent =
      _mesa_hash_table_search(&compilers_ht, dev->mtl_handle);
   if (ent != NULL) {
      struct kk_mtl_compiler *compiler = ent->data;
      --compiler->refcount;

      if (compiler->refcount == 0) {
         _mesa_hash_table_remove(&compilers_ht, ent);
         mtl_release(compiler->handle);
         ralloc_free(compiler);
      }
   }

   simple_mtx_unlock(&compilers_ht_lock);
}

DERIVE_HASH_TABLE(mtl_sampler_packed);

static VkResult
kk_init_sampler_heap(struct kk_device *dev, struct kk_sampler_heap *h)
{
   h->ht = mtl_sampler_packed_table_create(NULL);
   if (!h->ht)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* We optimistically size the table to fit the maximum number of samplers we
    * advertise. If this exceeds the hardware sampler limit, it is handled by
    * additional checks in `kk_sampler_heap_add_locked` */
   VkResult result = kk_query_table_init(dev, &h->table, MSL_MAX_SAMPLERS);

   if (result != VK_SUCCESS) {
      ralloc_free(h->ht);
      return result;
   }

   simple_mtx_init(&h->lock, mtx_plain);
   h->sparse_queries = dev->vk.enabled_features.sparseResidencyImage2D ||
                       dev->vk.enabled_features.sparseResidencyImage3D;
   return VK_SUCCESS;
}

static void
kk_destroy_sampler_heap(struct kk_device *dev, struct kk_sampler_heap *h)
{
   struct hash_entry *entry = _mesa_hash_table_next_entry(h->ht, NULL);
   while (entry) {
      struct kk_rc_sampler *sampler = (struct kk_rc_sampler *)entry->data;
      mtl_release(sampler->handle);
      entry = _mesa_hash_table_next_entry(h->ht, entry);
   }
   kk_sparse_sample_rows_finish(dev);
   kk_query_table_finish(dev, &h->table);
   ralloc_free(h->ht);
   simple_mtx_destroy(&h->lock);
   kk_sampler_cap_native_finish(dev);
}

static VkResult
kk_sampler_require_index(struct kk_device *dev, struct kk_sampler_heap *h,
                         struct kk_rc_sampler *rc)
{
   if (rc->index != UINT16_MAX)
      return VK_SUCCESS;
   const struct kk_physical_device *pdev = kk_device_physical(dev);
   if (h->indexed_count >= MIN2(pdev->info.max_sampler_count, MSL_MAX_SAMPLERS))
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   uint32_t index;
   VkResult result = kk_query_table_add(dev, &h->table,
      mtl_sampler_get_gpu_resource_id(rc->handle), &index);
   if (result == VK_SUCCESS) {
      rc->index = index;
      ++h->indexed_count;
   }
   return result;
}

static VkResult
kk_sampler_cap_fallback_locked(struct kk_device *dev, struct kk_sampler_heap *h,
   const struct mtl_sampler_packed *requested, struct kk_rc_sampler **out, bool needs_index)
{
   struct mtl_sampler_packed key;
   kk_sampler_cap_key(&key, requested);
   struct hash_entry *entry = _mesa_hash_table_search(h->ht, &key);
   if (!entry)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_rc_sampler *rc = entry->data;
   if (needs_index) {
      VkResult result = kk_sampler_require_index(dev, h, rc);
      if (result != VK_SUCCESS)
         return result;
   }
   ++rc->refcount;
   *out = rc;
   return VK_SUCCESS;
}

static VkResult
kk_sampler_heap_add_locked(struct kk_device *dev, struct kk_sampler_heap *h,
                           struct mtl_sampler_packed desc,
                           struct kk_rc_sampler **out, bool needs_index)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);

   kk_sampler_cap_key_normalize(&desc, &desc);
   struct hash_entry *ent = _mesa_hash_table_search(h->ht, &desc);
   if (ent != NULL) {
      *out = ent->data;

      if (needs_index) {
         VkResult result = kk_sampler_require_index(dev, h, *out);
         if (result != VK_SUCCESS)
            return result;
      }
      assert((*out)->refcount != 0);
      (*out)->refcount++;

      return VK_SUCCESS;
   }

   struct kk_rc_sampler *rc = ralloc(h->ht, struct kk_rc_sampler);
   if (!rc)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   mtl_sampler *handle = kk_sampler_create(dev, &desc);
   if (!handle) {
      ralloc_free(rc);
      if (h->canonical_count)
         return kk_sampler_cap_fallback_locked(dev, h, &desc, out, needs_index);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *rc = (struct kk_rc_sampler){
      .key = desc,
      .handle = handle,
      .refcount = 1,
      .index = UINT16_MAX,
   };

   if (needs_index) {
      VkResult result = kk_sampler_require_index(dev, h, rc);
      if (result != VK_SUCCESS) {
         mtl_release(handle);
         ralloc_free(rc);
         return result;
      }
   }
   _mesa_hash_table_insert(h->ht, &rc->key, rc);
   *out = rc;

   return VK_SUCCESS;
}

VkResult
kk_sampler_cap_reserve_locked(struct kk_device *dev)
{
   struct kk_sampler_heap *h = &dev->samplers;
   if (h->canonical_count)
      return VK_SUCCESS;

   /* No native state is needed before the first sampler or sampled sparse
    * view. Reserve the complete tap family before admitting acceleration. */
   assert(h->ht->entries == 0);
   VkResult result = kk_sampler_cap_native_init(dev);
   if (result != VK_SUCCESS)
      return result;
   const struct kk_sampler_native_state *states;
   uint32_t count = kk_sampler_cap_native_states(dev, &states);
   for (uint32_t i = 0; i < count; ++i) {
      /* The published canonical family is immutable. Bind it directly rather
       * than repeating key normalization, two hash lookups and a global lock
       * for every local reference. Cache ownership keeps each handle alive. */
      struct kk_rc_sampler *rc = rzalloc(h->ht, struct kk_rc_sampler);
      if (!rc) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }
      memcpy(&rc->key, &states[i].key, sizeof(rc->key));
      rc->handle = states[i].handle;
      mtl_retain(rc->handle);
      rc->refcount = 1;
      rc->index = UINT16_MAX;
      rc->canonical = true;
      result = kk_sampler_require_index(dev, h, rc);
      if (result == VK_SUCCESS &&
          !_mesa_hash_table_insert(h->ht, &rc->key, rc))
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      if (result != VK_SUCCESS) {
         if (rc->index != UINT16_MAX) {
            kk_query_table_remove(dev, &h->table, rc->index);
            --h->indexed_count;
         }
         mtl_release(rc->handle);
         ralloc_free(rc);
         goto fail;
      }
   }
   h->canonical_count = count;
   return VK_SUCCESS;

fail:
   /* No application handles were published. Other devices may still own the
    * shared cache; unwind only this device's complete or partial references. */
   hash_table_foreach(h->ht, entry) {
      struct kk_rc_sampler *partial = entry->data;
      if (partial->index != UINT16_MAX)
         kk_query_table_remove(dev, &h->table, partial->index);
      mtl_release(partial->handle);
      ralloc_free(partial);
   }
   _mesa_hash_table_clear(h->ht, NULL);
   h->indexed_count = 0;
   kk_sampler_cap_native_finish(dev);
   return result;
}

VkResult
kk_sampler_cap_get_canonical(struct kk_device *dev,
   const struct mtl_sampler_packed *requested, struct kk_rc_sampler **out, bool native_only)
{
   struct kk_sampler_heap *h = &dev->samplers;
   simple_mtx_lock(&h->lock);
   VkResult result = kk_sampler_cap_reserve_locked(dev);
   if (result == VK_SUCCESS)
      result = kk_sampler_cap_fallback_locked(dev, h, requested, out, !native_only);
   simple_mtx_unlock(&h->lock);
   return result;
}

static void kk_sampler_heap_remove_locked(struct kk_device *dev,
   struct kk_sampler_heap *h, struct kk_rc_sampler *rc);

VkResult
kk_sampler_heap_add(struct kk_device *dev, struct mtl_sampler_packed desc,
                    struct kk_rc_sampler **out)
{
   struct kk_sampler_heap *h = &dev->samplers;

   simple_mtx_lock(&h->lock);
   VkResult result = kk_sampler_cap_reserve_locked(dev);
   if (result == VK_SUCCESS)
      result = kk_sampler_heap_add_locked(dev, h, desc, out, true);
   if (result == VK_SUCCESS && h->sparse_queries &&
       kk_sampler_cap_keys_equal(&desc, &(*out)->key)) {
      result = kk_sparse_sampler_added(dev, *out);
      if (result != VK_SUCCESS) kk_sampler_heap_remove_locked(dev, h, *out);
   }
   simple_mtx_unlock(&h->lock);

   return result;
}

VkResult
kk_sampler_native_add(struct kk_device *dev, struct mtl_sampler_packed desc,
                       struct kk_rc_sampler **out)
{
   struct kk_sampler_heap *h = &dev->samplers;
   simple_mtx_lock(&h->lock);
   VkResult result = kk_sampler_cap_reserve_locked(dev);
   if (result == VK_SUCCESS)
      result = kk_sampler_heap_add_locked(dev, h, desc, out, false);
   if (result == VK_SUCCESS && h->sparse_queries &&
       kk_sampler_cap_keys_equal(&desc, &(*out)->key)) {
      result = kk_sparse_sampler_added(dev, *out);
      if (result != VK_SUCCESS) kk_sampler_heap_remove_locked(dev, h, *out);
   }
   simple_mtx_unlock(&h->lock);
   return result;
}

static void
kk_sampler_heap_remove_locked(struct kk_device *dev, struct kk_sampler_heap *h,
                              struct kk_rc_sampler *rc)
{
   assert(rc->refcount != 0);
   rc->refcount--;

   if (rc->refcount == 0) {
      if (h->sparse_queries)
         kk_sparse_sampler_removed(dev, rc);
      mtl_release(rc->handle);
      if (rc->index != UINT16_MAX) {
         kk_query_table_remove(dev, &h->table, rc->index);
         --h->indexed_count;
      }
      _mesa_hash_table_remove_key(h->ht, &rc->key);
      ralloc_free(rc);
   }
}

void
kk_sampler_heap_remove(struct kk_device *dev, struct kk_rc_sampler *rc)
{
   struct kk_sampler_heap *h = &dev->samplers;

   simple_mtx_lock(&h->lock);
   kk_sampler_heap_remove_locked(dev, h, rc);
   simple_mtx_unlock(&h->lock);
}

static VkResult
kk_get_timestamp(struct vk_device *device, uint64_t *timestamp)
{
   struct kk_device *dev = container_of(device, struct kk_device, vk);

   uint64_t gpu_ns = mtl_device_get_gpu_timestamp(dev->mtl_handle);
   uint64_t frequency = mtl_device_timestamp_frequency(dev->mtl_handle);

   *timestamp =
      (uint64_t)(((unsigned __int128)gpu_ns * frequency) / 1000000000ull);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_CreateDevice(VkPhysicalDevice physicalDevice,
                const VkDeviceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(kk_physical_device, pdev, physicalDevice);
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   struct kk_device *dev;

   dev = vk_zalloc2(&pdev->vk.instance->alloc, pAllocator, sizeof(*dev), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!dev)
      return vk_error(pdev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Fill the dispatch table we will expose to the users */
   dev->exposed_dispatch_table = kk_device_cmd_trampolines;
   vk_device_dispatch_table_from_entrypoints(&dev->exposed_dispatch_table,
                                             &kk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dev->exposed_dispatch_table,
                                             &wsi_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dev->exposed_dispatch_table, &vk_common_device_entrypoints, false);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &kk_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_common_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &wsi_device_entrypoints, false);

   result = vk_device_init(&dev->vk, &pdev->vk, &dispatch_table, pCreateInfo,
                           pAllocator);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   simple_mtx_init(&dev->depth_stencil_cache_mutex, mtx_plain);
   dev->perf = kk_perf_create();
   if (dev->perf) {
      dev->vk.pipeline_cache_lookup = kk_perf_cache_lookup;
      dev->vk.queue_submit_depth = kk_perf_submit_queue;
      kk_perf_init_dispatch(dev);
   }

   /* Metal cannot order a queue behind work that has not been encoded.
    * Start Mesa's submit worker only when a pending dependency requires it. */
   vk_device_enable_threaded_submit(&dev->vk);

   /* The original extension has no feature structure. Normalize its
    * request to the promoted feature used by the shader cache/lowering. */
   dev->vk.enabled_features.samplerFilterMinmax |=
      dev->vk.enabled_extensions.EXT_sampler_filter_minmax;

   dev->dgc_once = (util_once_flag)UTIL_ONCE_FLAG_INIT;
   kk_descriptor_heap_init(dev);
   result = kk_vrs_device_init(dev);
   if (result != VK_SUCCESS) goto fail_init;
   dev->vk.shader_ops = &kk_device_shader_ops;
   dev->mtl_handle = pdev->mtl_dev_handle;
   dev->vk.command_buffer_ops = &kk_cmd_buffer_ops;
   /* The extension dispatch is selected once; ordinary rendering has no
    * MSRTSS pNext walk or implicit attachment setup. */
   dev->whole_pass_begin_rendering =
      dev->vk.enabled_features.multisampledRenderToSingleSampled
         ? kk_CmdBeginRendering : kk_begin_rendering;
   dev->whole_pass_end_rendering = dev->vk.dispatch_table.CmdEndRendering2KHR;
   dev->vk.dispatch_table.CmdBeginRendering = kk_whole_pass_begin_rendering;
   dev->vk.dispatch_table.CmdEndRendering = kk_whole_pass_end_rendering;
   dev->vk.dispatch_table.CmdEndRendering2KHR = kk_whole_pass_end_rendering2;
   dev->vk.command_dispatch_table = &dev->vk.dispatch_table;
   dev->vk.get_timestamp = kk_get_timestamp;

   /* Create a new Metal pipeline compiler for the device */
   dev->mtl_compiler_handle = kk_acquire_compiler(dev);
   if (dev->mtl_compiler_handle == NULL)
      goto fail_init;

   /* We need to initialize the device residency set before any bo is created. */
   simple_mtx_init(&dev->residency_set.mutex, mtx_plain);
   list_inithead(&dev->residency_set.allocations);
   dev->residency_set.handle = mtl_new_residency_set(dev->mtl_handle);
   if (dev->residency_set.handle == NULL)
      goto fail_compiler;

   /* The optional set can fail into cleanup that destroys this mutex. */
   if (dev->vk.enabled_extensions.KHR_swapchain)
      simple_mtx_init(&dev->blit_queue_mutex, mtx_plain);
   result = kk_interlock_submission_init(dev);
   if (result != VK_SUCCESS)
      goto fail_vab_memory;
   list_inithead(&dev->residency_set.priority_memories);
   if (dev->vk.enabled_features.memoryPriority ||
       dev->vk.enabled_features.pageableDeviceLocalMemory) {
      dev->residency_set.priority_handle = mtl_new_residency_set(dev->mtl_handle);
      if (!dev->residency_set.priority_handle) {
         result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto fail_vab_memory;
      }
      mtl_residency_set_request_residency(dev->residency_set.priority_handle);
   }

   /* Weak entries reuse live shaders and their native Metal pipelines. */
   struct vk_pipeline_cache_create_info cache_info = {.weak_ref = true};
   dev->vk.mem_cache = vk_pipeline_cache_create(&dev->vk, &cache_info, NULL);
   if (dev->vk.mem_cache == NULL) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_mem_cache;
   }

   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
      const VkDeviceQueueCreateInfo *info = &pCreateInfo->pQueueCreateInfos[i];
      for (uint32_t q = 0; q < info->queueCount; ++q) {
         assert(dev->queue_count < ARRAY_SIZE(dev->queues));
         result = kk_queue_init(dev, &dev->queues[dev->queue_count], info, q);
         if (result != VK_SUCCESS)
            goto fail_mem_cache;
         ++dev->queue_count;
      }
   }

   result = kk_device_init_meta(dev);
   if (result != VK_SUCCESS)
      goto fail_mem_cache;

   result = kk_query_table_init(dev, &dev->occlusion_queries,
                                KK_MAX_OCCLUSION_QUERIES);
   if (result != VK_SUCCESS)
      goto fail_meta;

   result = kk_init_sampler_heap(dev, &dev->samplers);
   if (result != VK_SUCCESS)
      goto fail_query_table;

   result = kk_device_init_lib(dev);
   if (result != VK_SUCCESS)
      goto fail_sampler_heap;

   result = kk_interlock_device_init(dev);
   if (result != VK_SUCCESS)
      goto fail_lib;

   if (dev->vk.enabled_features.shaderImageInt64Atomics ||
       dev->vk.enabled_features.shaderBufferInt64Atomics ||
       dev->vk.enabled_features.shaderSharedInt64Atomics) {
      result = kk_alloc_bo(dev, &dev->vk.base, 65536 * sizeof(uint32_t), 64,
                           &dev->atomic64_locks);
      if (result != VK_SUCCESS) {
         kk_dgc_finish(dev);
   kk_device_finish_lib(dev);
         goto fail_sampler_heap;
      }
      memset(dev->atomic64_locks->cpu, 0, 65536 * sizeof(uint32_t));
   }

   if (pdev->settings.gpu_capture_enabled) {
      const char *capture_directory =
         debug_get_option("MESA_KK_GPU_CAPTURE_DIRECTORY", NULL);
      mtl_start_gpu_capture(dev->mtl_handle, capture_directory);
   }

   kk_frame_capture_init(dev);
   *pDevice = kk_device_to_handle(dev);

   return VK_SUCCESS;

fail_lib:
   kk_dgc_finish(dev);
   kk_device_finish_lib(dev);
fail_sampler_heap:
   kk_interlock_device_finish(dev);
   kk_destroy_sampler_heap(dev, &dev->samplers);
fail_query_table:
   kk_query_table_finish(dev, &dev->occlusion_queries);
fail_meta:
   kk_device_finish_meta(dev);
fail_mem_cache:
   vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);
   while (dev->queue_count > 0)
      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);
fail_vab_memory:
   kk_interlock_submission_finish(dev);
   if (dev->vk.enabled_extensions.KHR_swapchain)
      simple_mtx_destroy(&dev->blit_queue_mutex);
   if (dev->residency_set.priority_handle) {
      mtl_residency_set_end_residency(dev->residency_set.priority_handle);
      mtl_release(dev->residency_set.priority_handle);
   }
   mtl_release(dev->residency_set.handle);
   simple_mtx_destroy(&dev->residency_set.mutex);
fail_compiler:
   kk_release_compiler(dev);
fail_init:
   kk_depth_stencil_cache_finish(dev);
   kk_vrs_device_finish(dev);
   kk_descriptor_heap_finish(dev);
   kk_perf_destroy(dev->perf);
   vk_device_finish(&dev->vk);
fail_alloc:
   vk_free(&dev->vk.alloc, dev);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
kk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   struct kk_physical_device *pdev = kk_device_physical(dev);

   if (!dev)
      return;

   /* End capture before we start releasing resources. Otherwise, Metal capture
    * may run into issues. */
   if (pdev->settings.gpu_capture_enabled) {
      mtl_stop_gpu_capture();
   }

   /* Drain the private present queue before destroying shared meta state. */
   if (dev->blit_queue) {
      kk_queue_finish(dev, dev->blit_queue);
      vk_free(&dev->vk.alloc, dev->blit_queue);
      dev->blit_queue = NULL;
   }
   if (dev->vk.enabled_extensions.KHR_swapchain)
      simple_mtx_destroy(&dev->blit_queue_mutex);

   /* Host continuations may still encode using shared meta resources.
    * Join/cancel submit workers and drain their native callbacks first. */
   while (dev->queue_count > 0)
      kk_queue_finish(dev, &dev->queues[--dev->queue_count]);

   /* Common workers and per-queue providers are finished; close the retained
    * interlock registry before destroying shared meta/query/heap state. */
   kk_interlock_submission_drain(dev);
   /* Meta first since it may destroy Vulkan objects */
   kk_device_finish_meta(dev);
   kk_depth_stencil_cache_finish(dev);
   kk_dgc_finish(dev);
   kk_device_finish_lib(dev);
   kk_interlock_device_finish(dev);
   kk_vrs_device_finish(dev);
   kk_query_table_finish(dev, &dev->occlusion_queries);
   kk_descriptor_heap_finish(dev);
   kk_destroy_sampler_heap(dev, &dev->samplers);

   if (dev->atomic64_locks)
      kk_destroy_bo(dev, dev->atomic64_locks);

   /* Geometry heap */
   if (dev->heap)
      kk_destroy_bo(dev, dev->heap);

   kk_sparse_device_finish(dev);

   /* Release the residency set last once all BOs are released. */
   if (dev->residency_set.priority_handle) {
      mtl_residency_set_end_residency(dev->residency_set.priority_handle);
      mtl_release(dev->residency_set.priority_handle);
   }
   mtl_release(dev->residency_set.handle);
   simple_mtx_destroy(&dev->residency_set.mutex);

   vk_pipeline_cache_destroy(dev->vk.mem_cache, NULL);
   kk_release_compiler(dev);
   kk_interlock_submission_finish(dev);

   kk_perf_destroy(dev->perf);
   vk_device_finish(&dev->vk);

   vk_free(&dev->vk.alloc, dev);
}

/* We need to implement this ourselves so we give the fake ones for vk_common_*
 * to work when executing actual commands */
static PFN_vkVoidFunction
kk_device_get_proc_addr(const struct kk_device *device, const char *name)
{
   if (device == NULL || name == NULL)
      return NULL;

   struct vk_instance *instance = device->vk.physical->instance;
   return vk_device_dispatch_table_get_if_supported(
      &device->exposed_dispatch_table, name, instance->app_info.api_version,
      &instance->enabled_extensions, &device->vk.enabled_extensions);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
kk_GetDeviceProcAddr(VkDevice _device, const char *pName)
{
   VK_FROM_HANDLE(kk_device, device, _device);
   return kk_device_get_proc_addr(device, pName);
}

static mtl_allocation *
kk_bo_allocation(struct kk_bo *bo)
{
   return bo->mtl_handle ? (mtl_allocation *)bo->mtl_handle
                         : (mtl_allocation *)bo->map;
}

void
kk_device_add_bo_to_residency_set(struct kk_device *dev, struct kk_bo *bo)
{
   mtl_allocation *allocation = kk_bo_allocation(bo);
   simple_mtx_lock(&dev->residency_set.mutex);
   list_inithead(&bo->residency_alias_link);
   if (mtl_residency_set_contains_allocation(dev->residency_set.handle,
                                             allocation)) {
      /* Only imported native aliases need a lookup; ordinary new allocations
       * take the constant-time path and allocate no reference-count record. */
      list_for_each_entry(struct kk_bo, alias, &dev->residency_set.allocations,
                          residency_link) {
         if (kk_bo_allocation(alias) == allocation) {
            list_addtail(&bo->residency_alias_link, &alias->residency_alias_link);
            break;
         }
      }
   } else {
      mtl_residency_set_add_allocation(dev->residency_set.handle, allocation);
   }
   list_addtail(&bo->residency_link, &dev->residency_set.allocations);
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_remove_bo_from_residency_set(struct kk_device *dev, struct kk_bo *bo)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   bool last_alias = list_is_empty(&bo->residency_alias_link);
   list_del(&bo->residency_alias_link);
   list_del(&bo->residency_link);
   if (last_alias)
      mtl_residency_set_remove_allocation(dev->residency_set.handle,
                                          kk_bo_allocation(bo));
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_add_heap_to_residency_set(struct kk_device *dev, mtl_heap *heap)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   mtl_residency_set_add_allocation(dev->residency_set.handle, heap);
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_remove_heap_from_residency_set(struct kk_device *dev, mtl_heap *heap)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   mtl_residency_set_remove_allocation(dev->residency_set.handle, heap);
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_add_buffer_to_residency_set(struct kk_device *dev, mtl_buffer *buffer)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   mtl_residency_set_add_allocation(dev->residency_set.handle, buffer);
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_remove_buffer_from_residency_set(struct kk_device *dev,
                                           mtl_buffer *buffer)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   mtl_residency_set_remove_allocation(dev->residency_set.handle, buffer);
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_make_resources_resident(struct kk_device *dev)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   if (dev->residency_set.dirty) {
      KK_PERF_VOID(dev, KK_PERF_EXECUTION_RESIDENCY_COMMIT,
         mtl_residency_set_commit(dev->residency_set.handle));
      dev->residency_set.dirty = false;
   }
   /* Every Metal queue already uses this set for execution-scoped residency.
    * Pageable mode leaves idle pages to the OS instead of requesting lifetime
    * residency for every allocation. The priority set requests extra retention. */
   /* A residency request persists until endResidency. Subsequent commits
    * propagate membership changes to the already resident set. */
   if (!dev->vk.enabled_features.pageableDeviceLocalMemory &&
       !dev->residency_set.requested) {
      mtl_residency_set_request_residency(dev->residency_set.handle);
      dev->residency_set.requested = true;
   }
   simple_mtx_unlock(&dev->residency_set.mutex);
}

/* VK_EXT_debug_utils */
VKAPI_ATTR VkResult VKAPI_CALL
kk_SetDebugUtilsObjectNameEXT(VkDevice device,
                              const VkDebugUtilsObjectNameInfoEXT *pNameInfo)
{
   VkResult result = vk_common_SetDebugUtilsObjectNameEXT(device, pNameInfo);
   if (result != VK_SUCCESS)
      return result;

   if (pNameInfo->pObjectName == NULL)
      return result;

#define CASE(obj_type, kk_type, vk_type)                                       \
   case VK_OBJECT_TYPE_##obj_type: {                                           \
      VK_FROM_HANDLE(kk_type, obj, (vk_type)pNameInfo->objectHandle);          \
      kk_type##_set_label(obj, pNameInfo->pObjectName);                        \
      break;                                                                   \
   }

   switch (pNameInfo->objectType) {
      CASE(COMMAND_BUFFER, kk_cmd_buffer, VkCommandBuffer);
      CASE(COMMAND_POOL, kk_cmd_pool, VkCommandPool);
      CASE(DEVICE_MEMORY, kk_device_memory, VkDeviceMemory);
      CASE(BUFFER, kk_buffer, VkBuffer);
      CASE(BUFFER_VIEW, kk_buffer_view, VkBufferView);
      CASE(IMAGE, kk_image, VkImage);
      CASE(IMAGE_VIEW, kk_image_view, VkImageView);
      CASE(DESCRIPTOR_POOL, kk_descriptor_pool, VkDescriptorPool);
      /* TODO: it would be nice to label pipelines and shaders, but we don't
       * have easy access to the relevant vulkan runtime objects inside
       * kk_compile_shaders(), where the Metal objects are set up.
       */
   default:
      break;
   }
#undef CASE

   return VK_SUCCESS;
}

#if DETECT_OS_APPLE
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
   return kk_GetDeviceProcAddr(device, name);
}
#endif
