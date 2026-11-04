/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_PRIVATE_H
#define KK_PRIVATE_H 1

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <stdbool.h>

/* Deferred compute builds run on compiler threads, where the application's
 * debug messengers must not be called (under Wine they are PE callbacks).
 * Code that a build runs reports errors with kk_error and kk_errorf, which
 * return the result without logging during a build; the API thread that
 * joins the build reports its result. */
extern _Thread_local bool kk_compile_worker;
#define kk_error(obj, error) \
   (kk_compile_worker ? (error) : vk_error(obj, error))
#define kk_errorf(obj, error, ...) \
   (kk_compile_worker ? (error) : vk_errorf(obj, error, __VA_ARGS__))

#define KK_MAX_SETS                      32
#define KK_MAX_PUSH_SIZE                 4096
#define KK_MAX_DYNAMIC_BUFFERS           128
#define KK_MAX_RTS                       8
#define KK_MAX_SAMPLES                   8
/* Fixed VK_KHR_fragment_shading_rate attachment texel size. */
#define KK_FSR_TEXEL_SIZE                8
/* Per-fragment slots of multisampled shading-rate fragments. */
#define KK_FSR_SLOT_BYTES                (64u << 20)
#define KK_MIN_SSBO_ALIGNMENT            4
#define KK_MIN_TEXEL_BUFFER_ALIGNMENT    16
#define KK_MIN_UBO_ALIGNMENT             64
#define KK_MAX_VIEWPORTS                 16
#define KK_MAX_DESCRIPTOR_SIZE           64
#define KK_MAX_PUSH_DESCRIPTORS          256
#define KK_MAX_DESCRIPTOR_SET_SIZE       (1u << 30)
#define KK_MAX_INLINE_UNIFORM_BLOCK_SIZE (1u << 16)
#define KK_MAX_DESCRIPTORS               (1 << 20)
#define KK_PUSH_DESCRIPTOR_SET_SIZE                                            \
   (KK_MAX_PUSH_DESCRIPTORS * KK_MAX_DESCRIPTOR_SIZE)
#define KK_SSBO_BOUNDS_CHECK_ALIGNMENT 4
#define KK_MAX_MULTIVIEW_VIEW_COUNT    32
/* Mesh multiview replays the complete Mesh draw once per view. Eight views
 * cover D3D12 view instancing (four views) and stay below 32, where
 * dEQP-VK.mesh_shader.ext.properties.max_view_index builds its view mask as
 * (1u << count) - 1 and the shift by 32 yields an empty mask. */
#define KK_MAX_MESH_MULTIVIEW_VIEW_COUNT 8
#define KK_TEXTURE_BUFFER_WIDTH        (1u << 14)
#define KK_MAX_OCCLUSION_QUERIES       (32768)
#define KK_MIN_SAMPLE_LOCATION         (0.0f)
#define KK_MAX_SAMPLE_LOCATION         (0.9375f)

#define KK_SPARSE_ADDR_SPACE_SIZE (1ull << 39)

/* Max size of a bound cbuf */
#define KK_MAX_CBUF_SIZE (1u << 16)

/* Metal related macros */
#define KK_MTL_RESOURCE_OPTIONS                                                \
   MTL_RESOURCE_STORAGE_MODE_SHARED |                                          \
      MTL_RESOURCE_CPU_CACHE_MODE_DEFAULT_CACHE |                              \
      MTL_RESOURCE_TRACKING_MODE_UNTRACKED

struct kk_addr_range {
   uint64_t addr;
   uint64_t range;
};

#endif
