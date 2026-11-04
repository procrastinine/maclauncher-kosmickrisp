/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef MTL_BUFFER_H
#define MTL_BUFFER_H 1

#include "mtl_types.h"

#include <inttypes.h>

struct kk_image_layout;

/* Utils */
uint64_t mtl_buffer_get_length(mtl_buffer *buffer);
uint64_t mtl_buffer_get_gpu_address(mtl_buffer *buffer);
/* Gets CPU address */
void *mtl_get_contents(mtl_buffer *buffer);

/* Zero the complete buffer and wait before exposing the allocation. */
enum mtl_command_queue_error
mtl_buffer_zero_sync(mtl_device *device, mtl_buffer *buffer);

/* FD and mapping ownership transfers only on a successful native allocation. */
mtl_buffer *mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
   uint64_t mapping_size, uint64_t payload_offset, uint64_t payload_size);

/* Allocation from buffer */
mtl_texture *mtl_new_texture_with_descriptor_linear(
   mtl_buffer *buffer, const struct kk_image_layout *layout, uint64_t offset);

#endif /* MTL_BUFFER_H */