/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_buffer.h"

uint64_t
mtl_buffer_get_length(mtl_buffer *buffer)
{
   return 0u;
}

uint64_t
mtl_buffer_get_gpu_address(mtl_buffer *buffer)
{
   return 0u;
}

void *
mtl_get_contents(mtl_buffer *buffer)
{
   return NULL;
}

mtl_texture *
mtl_new_texture_with_descriptor_linear(mtl_buffer *buffer,
                                       const struct kk_image_layout *layout,
                                       uint64_t offset)
{
   return NULL;
}

mtl_buffer *
mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
   uint64_t mapping_size, uint64_t payload_offset, uint64_t payload_size)
{
   return NULL;
}
