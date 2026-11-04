/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef MTL_BUFFER_H
#define MTL_BUFFER_H 1

#include "mtl_types.h"

#include <inttypes.h>
#include <stdbool.h>

struct kk_image_layout;

/* Utils */
uint64_t mtl_buffer_get_length(mtl_buffer *buffer);
uint64_t mtl_buffer_get_gpu_address(mtl_buffer *buffer);
/* Prove equal storage intervals from live native resources, never GPU VAs.
 * Offsets are buffer-relative; callers keep both resources valid. */
bool mtl_buffer_ranges_alias(mtl_buffer *a, uint64_t a_offset,
                             mtl_buffer *b, uint64_t b_offset, uint64_t size);
/* Gets CPU address */
void *mtl_get_contents(mtl_buffer *buffer);

/* Zero the complete buffer and wait before exposing the allocation. */
enum mtl_command_queue_error
mtl_buffer_zero_sync(mtl_command_queue *queue, mtl_buffer *buffer);
/* Legacy blit queue, created only on the first private zeroed allocation. */
mtl_command_queue *mtl_new_zero_fill_queue(mtl_device *device);

/* FD and mapping ownership transfers only on a successful native allocation. */
mtl_buffer *mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
   uint64_t mapping_size, uint64_t payload_offset, uint64_t payload_size);

/* Allocation from buffer */
mtl_texture *mtl_new_texture_with_descriptor_linear(
   mtl_buffer *buffer, const struct kk_image_layout *layout, uint64_t offset);

#endif /* MTL_BUFFER_H */