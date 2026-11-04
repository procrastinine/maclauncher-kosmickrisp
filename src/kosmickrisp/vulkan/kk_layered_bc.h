/* SPDX-License-Identifier: MIT */
#ifndef KK_LAYERED_BLOCK_VIEWS_H
#define KK_LAYERED_BLOCK_VIEWS_H
#include "kk_sparse.h"
#include "kk_native_block_alias.h"

/* Query records share the actual sparse prefix, including future appended
 * native IDs. The layer tables follow that prefix rather than fixed offsets. */
struct kk_layered_bc_record {
   struct kk_sparse_query_record query;
   uint64_t sampled_descriptors;
   uint64_t storage_descriptors;
   uint32_t layer_count;
   uint32_t reserved;
};
static_assert(offsetof(struct kk_sparse_query_record, kind) == 60,
              "query record kind offset");
static_assert(offsetof(struct kk_layered_bc_record, sampled_descriptors) ==
              sizeof(struct kk_sparse_query_record), "layer table follows query prefix");
static_assert(offsetof(struct kk_layered_bc_record, storage_descriptors) ==
              sizeof(struct kk_sparse_query_record) + 8, "storage table offset");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) == 20,
              "image LOD and view-kind bits");
static_assert(sizeof(struct kk_sampled_image_descriptor) == 64, "sampled descriptor size");
static_assert(sizeof(struct kk_storage_image_descriptor) == 16, "storage descriptor size");
static_assert(offsetof(struct kk_storage_image_descriptor, image_array_gpu_resource_id) ==
              sizeof(uint64_t), "ordinary storage array ID follows plain ID");
struct kk_image_view;
struct kk_layered_bc_view;
struct kk_attachment;
struct kk_cmd_buffer;
struct kk_rendering_state;

bool kk_layered_bc_requested(const struct vk_image_view *view);
VkResult kk_layered_bc_view_init(struct kk_device *dev, struct kk_image_view *view);
void kk_layered_bc_view_finish(struct kk_device *dev, struct kk_image_view *view);
void kk_layered_bc_sampled_descriptor(struct kk_sampled_image_descriptor *desc,
                                     const struct kk_image_view *view);
void kk_layered_bc_storage_descriptor(struct kk_storage_image_descriptor *desc,
                                     const struct kk_image_view *view);
bool kk_layered_bc_rendering_info_requested(const VkRenderingInfo *info);
/* Callback data is &cmd->state.gfx.render and lives through draw cleanup.
 * The original view/memory and cached outputs outlive every raster reader. */
VkResult kk_layered_bc_bind_layer(struct kk_cmd_buffer *cmd, void *data,
                                uint32_t logical_layer);
void kk_layered_bc_attachment_description(
   mtl_render_pass_attachment_descriptor *descriptor,
   const struct kk_attachment *attachment,
   const struct kk_rendering_state *render, bool resolve);
#endif
