/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_PHYSICAL_DEVICE_H
#define KK_PHYSICAL_DEVICE_H 1

#include "kk_private.h"
#include "util/mesa-blake3.h"
#include "util/u_call_once.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_physical_device.h"
#include "vk_sync.h"
#include "vk_sync_binary.h"

#include "wsi_common.h"

#include <sys/types.h>

/* VkPhysicalDeviceLimits::subPixelPrecisionBits */
#define KK_SUBPIXEL_PRECISION_BITS 8

struct kk_instance;
struct kk_physical_device;

struct kk_queue_family {
   VkQueueFlags queue_flags;
   uint32_t queue_count;
};

struct kk_memory_heap {
   uint64_t size;
   VkMemoryHeapFlags flags;
   uint64_t (*budget)(struct kk_physical_device *pdev);
   uint64_t (*used)(struct kk_physical_device *pdev);
};

struct kk_device_info {
   uint32_t max_workgroup_count[3];
   uint32_t max_workgroup_invocations;
   uint32_t max_compute_shared_memory_size;
   uint32_t gpu_apple_family;
   bool placement_sparse;
   enum mtl_language_version msl_version;
   uint64_t max_buffer_size;
   uint32_t max_sampler_count;
   uint32_t rendering_tile_width;
   uint32_t rendering_tile_height;
   VkSampleCountFlags supported_sample_counts;
   uint32_t rgb32_texel_alignment[3]; /* Metal RGB32Uint, Sint, Float. */
   bool supports_32bit_msaa;
   bool supports_query_texture_lod;
   bool supports_bc_texture_compression;
   bool supports_32_bit_float_filtering;
};

struct kk_env_settings {
   bool experimental_enabled;
   bool gpu_capture_enabled;
   bool gpu_capture_immediately;
   const char *gpu_capture_dir;
   uint64_t disabled_workarounds;
   uint32_t apple_family_override; /* MESA_KK_APPLE_FAMILY, 0 when unset */
};

struct kk_physical_device {
   struct vk_physical_device vk;
   mtl_device *mtl_dev_handle;
   struct kk_device_info info;
   struct kk_env_settings settings;
   /* KK_EMULATION precise_timestamps: compute timestamps wait for earlier
    * dispatches instead of sharing the encoder's relaxed sample. */
   bool precise_timestamps;

   struct wsi_device wsi_device;

   uint8_t device_uuid[VK_UUID_SIZE];

   // TODO: add mapable VRAM heap if possible
   struct kk_memory_heap mem_heaps[3];
   VkMemoryType mem_types[3];
   uint8_t mem_heap_count;
   uint8_t mem_type_count;

   // Emulated binary sync type
   struct vk_sync_binary_type sync_binary_type;
   const struct vk_sync_type *sync_types[3];

   struct kk_queue_family queue_families[4];
   uint8_t queue_family_count;

   /* Immutable existing namespace; opening the store waits for actual use. */
   blake3_hash disk_cache_digest;
   util_once_flag disk_cache_once;
   /* Metal archive namespace: content-keyed, independent of the driver build. */
   blake3_hash archive_cache_digest;
};

static inline uint32_t
kk_min_cbuf_alignment()
{
   /* Size of vec4 */
   return 16;
}

VK_DEFINE_HANDLE_CASTS(kk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

static inline struct kk_instance *
kk_physical_device_instance(struct kk_physical_device *pdev)
{
   return (struct kk_instance *)pdev->vk.instance;
}

void kk_physical_device_ensure_disk_cache(struct kk_physical_device *pdev);

struct disk_cache *kk_create_disk_cache(const blake3_hash driver_digest,
   uint32_t apple_family, uint64_t disabled_workarounds);

VkResult kk_enumerate_physical_devices(struct vk_instance *_instance);
void kk_physical_device_destroy(struct vk_physical_device *vk_device);

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) ||                                    \
   defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR) ||    \
   defined(VK_USE_PLATFORM_DISPLAY_KHR) || defined(VK_USE_PLATFORM_METAL_EXT)
#define KK_USE_WSI_PLATFORM
#endif

#endif // KK_PHYSICAL_DEVICE_H
