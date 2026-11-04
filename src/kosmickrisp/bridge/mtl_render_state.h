/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef MTL_RENDER_STATE_H
#define MTL_RENDER_STATE_H 1

#include "mtl_types.h"

#include <inttypes.h>
#include <stdbool.h>

/* Bridge enums */
enum mtl_pixel_format;

/* TODO_KOSMICKRISP Remove */
enum VkCompareOp;
enum VkStencilOp;

/* Render pass descriptor */
#define MTL_RENDER_PASS_INFO_ATTACHMENTS 10

/* Native render-pass snapshot: color slots 0..7, depth 8, stencil 9. */
struct mtl_render_pass_attachment_info {
   bool present;
   uint32_t format;
   uint32_t samples;
   enum mtl_load_action load;
   enum mtl_store_action store;
   const void *texture, *base_texture;
};

struct mtl_render_pass_info {
   uint32_t default_samples;
   struct mtl_render_pass_attachment_info
      attachments[MTL_RENDER_PASS_INFO_ATTACHMENTS];
   uint32_t width, height;
};

/* Read-only snapshot; callers opt in before requesting native properties. */
void mtl_render_pass_descriptor_get_info(mtl_render_pass_descriptor *descriptor,
                                          struct mtl_render_pass_info *info);

mtl_render_pass_descriptor *mtl_new_render_pass_descriptor(void);
/* Recycle only inactive attachments; preserve the descriptor's child objects. */
void mtl_render_pass_descriptor_reuse(mtl_render_pass_descriptor *descriptor,
   uint8_t old_colors, uint8_t new_colors, bool old_depth, bool new_depth,
   bool old_stencil, bool new_stencil, mtl_buffer *visibility);

mtl_render_pass_attachment_descriptor *
mtl_render_pass_descriptor_get_color_attachment(
   mtl_render_pass_descriptor *descriptor, uint32_t index);

mtl_render_pass_attachment_descriptor *
mtl_render_pass_descriptor_get_depth_attachment(
   mtl_render_pass_descriptor *descriptor);

mtl_render_pass_attachment_descriptor *
mtl_render_pass_descriptor_get_stencil_attachment(
   mtl_render_pass_descriptor *descriptor);

void mtl_render_pass_attachment_descriptor_set_common(
   mtl_render_pass_attachment_descriptor *descriptor, mtl_texture *texture,
   uint32_t level, uint32_t slice, bool set_subresource,
   enum mtl_load_action load_action);

void mtl_render_pass_attachment_descriptor_set_texture(
   mtl_render_pass_attachment_descriptor *descriptor, mtl_texture *texture);

void mtl_render_pass_attachment_descriptor_set_level(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t level);

void mtl_render_pass_attachment_descriptor_set_slice(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t slice);

void mtl_render_pass_attachment_descriptor_set_load_action(
   mtl_render_pass_attachment_descriptor *descriptor,
   enum mtl_load_action action);

void mtl_render_pass_attachment_descriptor_set_store_action(
   mtl_render_pass_attachment_descriptor *descriptor,
   enum mtl_store_action action);

void mtl_render_pass_attachment_descriptor_set_clear_color(
   mtl_render_pass_attachment_descriptor *descriptor,
   struct mtl_clear_color clear_color);

void mtl_render_pass_attachment_descriptor_set_clear_depth(
   mtl_render_pass_attachment_descriptor *descriptor, double depth);

void mtl_render_pass_attachment_descriptor_set_clear_stencil(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t stencil);

void mtl_render_pass_attachment_descriptor_set_depth_resolve_filter(
   mtl_render_pass_attachment_descriptor *descriptor,
   enum mtl_depth_resolve_filter filter);

void mtl_render_pass_attachment_descriptor_set_stencil_resolve_filter(
   mtl_render_pass_attachment_descriptor *descriptor,
   enum mtl_stencil_resolve_filter filter);

void mtl_render_pass_attachment_descriptor_set_resolve_texture(
   mtl_render_pass_attachment_descriptor *descriptor,
   mtl_texture *resolve_texture);

void mtl_render_pass_attachment_descriptor_set_resolve_level(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t resolve_level);

void mtl_render_pass_attachment_descriptor_set_resolve_slice(
   mtl_render_pass_attachment_descriptor *descriptor, uint32_t resolve_slice);

void mtl_render_pass_attachment_descriptor_set_resolve_depth_plane(
   mtl_render_pass_attachment_descriptor *descriptor,
   uint32_t resolve_depth_plane);

void mtl_render_pass_descriptor_set_render_target_array_length(
   mtl_render_pass_descriptor *descriptor, uint32_t length);

void mtl_render_pass_descriptor_set_render_target_width(
   mtl_render_pass_descriptor *descriptor, uint32_t width);

void mtl_render_pass_descriptor_set_render_target_height(
   mtl_render_pass_descriptor *descriptor, uint32_t height);

void mtl_render_pass_descriptor_set_default_raster_sample_count(
   mtl_render_pass_descriptor *descriptor, uint32_t sample_count);

void mtl_render_pass_descriptor_set_sample_positions(
   mtl_render_pass_descriptor *descriptor,
   const struct mtl_sample_position *positions, uint32_t count);

void mtl_render_pass_descriptor_set_visibility_buffer(
   mtl_render_pass_descriptor *descriptor, mtl_buffer *visibility_buffer);

/* Stencil descriptor */
mtl_stencil_descriptor *mtl_new_stencil_descriptor(void);

void mtl_stencil_descriptor_set_stencil_failure_operation(
   mtl_stencil_descriptor *descriptor, enum VkStencilOp op);

void mtl_stencil_descriptor_set_depth_failure_operation(
   mtl_stencil_descriptor *descriptor, enum VkStencilOp op);

void mtl_stencil_descriptor_set_depth_stencil_pass_operation(
   mtl_stencil_descriptor *descriptor, enum VkStencilOp op);

void mtl_stencil_descriptor_set_stencil_compare_function(
   mtl_stencil_descriptor *descriptor, enum VkCompareOp op);

void mtl_stencil_descriptor_set_read_mask(mtl_stencil_descriptor *descriptor,
                                          uint32_t mask);

void mtl_stencil_descriptor_set_write_mask(mtl_stencil_descriptor *descriptor,
                                           uint32_t mask);

/* Depth stencil descriptor */
mtl_depth_stencil_descriptor *mtl_new_depth_stencil_descriptor(void);

void mtl_depth_stencil_descriptor_set_depth_compare_function(
   mtl_depth_stencil_descriptor *descriptor, enum VkCompareOp op);

void mtl_depth_stencil_descriptor_set_depth_write_enabled(
   mtl_depth_stencil_descriptor *descriptor, bool enable_write);

void mtl_depth_stencil_descriptor_set_back_face_stencil(
   mtl_depth_stencil_descriptor *descriptor,
   mtl_stencil_descriptor *stencil_descriptor);

void mtl_depth_stencil_descriptor_set_front_face_stencil(
   mtl_depth_stencil_descriptor *descriptor,
   mtl_stencil_descriptor *stencil_descriptor);

/* Depth stencil state */
mtl_depth_stencil_state *
mtl_new_depth_stencil_state(mtl_device *device,
                            mtl_depth_stencil_descriptor *descriptor);

#endif /* MTL_RENDER_STATE_H */
