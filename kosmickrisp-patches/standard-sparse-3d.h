/* SPDX-License-Identifier: MIT */
#ifndef KK_STANDARD_SPARSE_3D_H
#define KK_STANDARD_SPARSE_3D_H
#include "kk_standard_sparse_3d_layout.h"
#include "kk_native_block_alias.h"
struct kk_standard_3d_backing {
   mtl_heap *heap;
   uint32_t references;
};
struct kk_standard_3d_target {
   struct kk_standard_3d_target *next;
   struct kk_view_layout layout;
   struct kk_layered_bc_layer native;
};
struct kk_standard_3d_format {
   struct kk_standard_3d_format *next;
   uint32_t format;
   struct kk_layered_bc_layer native;
};
struct kk_standard_3d_page {
   struct kk_standard_3d_page *next;
   uint64_t offset;
   VkExtent3D shape;
   VkImage image;
   VkDeviceMemory memory;
   simple_mtx_t mutex;
   struct kk_standard_3d_target *targets;
   struct kk_standard_3d_format *formats;
};
struct kk_standard_3d_view_state {
   struct kk_standard_3d_view_state *next;
   struct kk_image_view *view;
   struct kk_standard_3d_image *image_state;
   struct kk_view_layout layout[2];
   mtl_buffer *targets[2];
};
struct kk_standard_3d_image {
   int references;
   mtl_buffer *table;
   struct kk_standard_3d_tile *tiles;
   VkDeviceMemory *memory;
   uint64_t *memory_offset;
   struct kk_standard_3d_backing *native;
   VkExtent3D native_granularity;
   uint32_t native_first_tail;
   uint64_t native_level_page[KK_STANDARD_3D_MIP_LEVELS];
   uint64_t native_pages, native_tail_pages;
   mtl_heap *native_tail_heap;
   uint32_t native_tail_references;
   uint64_t tile_count;
   simple_mtx_t mutex;
   struct kk_standard_3d_page **pages;
   struct kk_standard_3d_view_state *views;
};
static void kk_standard_3d_page_finish(struct kk_device *,struct kk_standard_3d_page *);
static VkResult kk_standard_3d_image_initialize(struct kk_device *,struct kk_image *);
static VkExtent3D kk_standard_3d_shape(enum pipe_format);
static void kk_standard_3d_image_finish(struct kk_device *,struct kk_image *);
static uint32_t kk_standard_3d_query_stride(const struct kk_image_view *);
static uint32_t kk_standard_3d_query_bytes(const struct kk_image *);
static VkResult kk_standard_3d_query_initialize(struct kk_image_view *,bool,const struct kk_view_layout *);
static void kk_standard_3d_query_finish(struct kk_device *,struct kk_image_view *);
static mtl_heap *kk_standard_3d_new_heap(struct kk_device *,uint64_t);
static mtl_texture *kk_standard_3d_physical_target(struct kk_device *,
   struct kk_standard_3d_page *,const struct kk_view_layout *,const struct kk_image_layout *);
VkResult kk_standard_3d_raw_view(struct kk_device *,struct kk_image_view *,const struct kk_view_layout *,mtl_texture **);
struct kk_sparse_texture_batch;
static bool kk_standard_3d_bind_images(struct kk_queue *,struct kk_sparse_texture_batch *,struct util_dynarray *,struct kk_image *,uint32_t,const VkSparseImageMemoryBind *);
static bool kk_standard_3d_bind_opaque(struct kk_queue *,struct kk_sparse_texture_batch *,struct util_dynarray *,struct kk_image *,uint32_t,const VkSparseMemoryBind *);
#endif
