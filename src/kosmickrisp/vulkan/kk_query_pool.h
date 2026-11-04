/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_QUERY_POOL_H
#define KK_QUERY_POOL_H 1

#include "kk_private.h"

#include "vulkan/runtime/vk_query_pool.h"

/* A timestamp pool takes its Metal entries from the device's timestamp arena in
 * units of KK_TIMESTAMP_UNIT_QUERIES queries. The arena holds at most
 * KK_TIMESTAMP_ARENA_HEAPS heaps; AGX allows a process 32. */
#define KK_TIMESTAMP_UNIT_QUERIES 32
#define KK_TIMESTAMP_ARENA_HEAPS 64

/* Where KK_TIMESTAMP_UNIT_QUERIES consecutive queries of a timestamp pool sample. */
struct kk_ts_chunk {
   mtl_counter_heap *heap;
   uint32_t first;
};

struct kk_ts_state {
   /* Metal 4 counter heap entries backing VK_QUERY_TYPE_TIMESTAMP pools, taken
    * from the device's timestamp arena (kk_timestamp_heaps.h). Timestamps are
    * sampled into them and later resolved into `bo` (see kk_query_pool.c).
    * chunk_count chunks are followed by their arena units. */
   struct kk_ts_chunk *chunks;
   uint32_t chunk_count;
};

struct kk_oq_state {
   unsigned queries;
};

struct kk_query_pool {
   struct vk_query_pool vk;

   struct kk_bo *bo;

   uint32_t index_start;
   uint32_t query_start;
   uint32_t query_stride;

   union {
      struct kk_ts_state ts;
      struct kk_oq_state oq;
      /* Packed report offsets fit the existing timestamp-state union. */
      uint8_t statistic_offsets[13];
   };
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_query_pool, vk.base, VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)

uint16_t *kk_pool_index_ptr(const struct kk_query_pool *pool);

#endif /* KK_QUERY_POOL_H */
