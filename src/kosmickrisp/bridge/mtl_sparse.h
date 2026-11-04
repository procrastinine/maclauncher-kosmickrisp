/* SPDX-License-Identifier: MIT */
#ifndef MTL_SPARSE_H
#define MTL_SPARSE_H
#include "mtl_types.h"
#include "mtl_command_queue.h"
#include <stdbool.h>
#include <stdint.h>
struct kk_image_layout;
struct mtl_sparse_buffer_mapping {
   size_t mode, first, count, offset;
};
struct mtl_sparse_texture_mapping {
   size_t mode;
   struct mtl_size origin, size;
   size_t level, layer, offset;
};
struct mtl_sparse_residency_copy {
   mtl_buffer *source, *destination;
   uint64_t destination_offset, size;
};
bool mtl_device_supports_placement_sparse(mtl_device *device);
bool mtl_sparse_commit_mapping_barrier(mtl_device *device, mtl_command_queue *queue,
   mtl_command_buffer **command, mtl_command_allocator **allocator,
   const struct mtl_sparse_residency_copy *copies, uint32_t copy_count,
   mtl_feedback_handler_callback callback, void *data);
mtl_buffer *mtl_sparse_new_residency_buffer(mtl_device *device, uint64_t size,
                                          bool resident);
struct mtl_size mtl_sparse_granularity(mtl_device *device, uint32_t format,
                                     enum mtl_texture_type type);
mtl_buffer *mtl_new_sparse_buffer(mtl_device *device, uint64_t size);
mtl_texture *mtl_new_sparse_texture(mtl_device *device, const struct kk_image_layout *layout);
uint64_t mtl_sparse_tail_size(mtl_texture *texture);
void *mtl_new_sparse_buffer_view(mtl_buffer *buffer, const struct kk_image_layout *layout,
                                uint64_t offset, uint64_t *resource_id);
void mtl_sparse_map_buffer_batch(mtl_command_queue *queue, mtl_buffer *buffer,
   mtl_heap *heap, const struct mtl_sparse_buffer_mapping *ops, uint32_t count);
void mtl_sparse_map_texture_batch(mtl_command_queue *queue, mtl_texture *texture,
   mtl_heap *heap, const struct mtl_sparse_texture_mapping *ops, uint32_t count);
#endif
