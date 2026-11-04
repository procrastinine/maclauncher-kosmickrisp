/* SPDX-License-Identifier: MIT */
#ifndef KK_DGC_H
#define KK_DGC_H
#include "kosmickrisp/bridge/mtl_types.h"
#include "kosmickrisp/libkk/kk_argument_table.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* Shared byte layout with kk_dgc.metal. Offsets are derived from the actual
 * Kosmic root/per-draw structures, not duplicated host structure definitions.
 */
struct kk_dgc_patch {
   uint32_t src, dst, size, sequence;
};
struct kk_dgc_attribute {
   uint32_t src, slot, binding, offset, size, dynamic_stride;
};
struct kk_dgc_params {
   uint64_t stream, count, output, base_root, base_draw, patches, attributes;
   uint64_t samplers, index, condition;
   uint32_t maximum, first, stream_stride, record_stride;
   uint32_t root_size, draw_offset, args_offset, index_offset;
   uint32_t patch_count, attribute_count, action_offset, index_token;
   uint32_t root_push, root_strides, root_bases, root_clamps;
   uint32_t draw_base_vertex, draw_base_instance;
   uint32_t action, primitive, index_size, dx_index;
   uint32_t local[3], inverted;
   uint32_t ordinary, index_range;
   uint64_t execution_range;
   uint32_t root_vertex_input, vertex_input_size, vertex_input_offset;
   uint32_t vertex_fetch_size, vertex_fetch_stride;
   uint64_t linked_vs_buffer, linked_fs_buffer;
   uint64_t interlock_guard;
};
static_assert(sizeof(struct kk_dgc_params) == 248, "DGC parameter ABI");
struct kk_device;
struct kk_cmd_buffer;
struct kk_dgc_metal;
struct kk_shader;
struct kk_root_descriptor_table;
struct msl_compile_data;
mtl_render_pipeline_descriptor *kk_dgc_render_descriptor(mtl_render_pipeline_descriptor *source);
mtl_compute_pipeline_state *kk_dgc_new_compute_pipeline_state(mtl_compiler *compiler, mtl_function_descriptor *function, uint64_t threads);
mtl_compute_pipeline_state *kk_dgc_get_compute_pipeline(struct kk_device *dev, struct kk_shader *shader);
VkResult kk_compile_indirect_compute_pipeline(struct kk_device *device, const struct msl_compile_data *data, uint32_t threads, mtl_compute_pipeline_state **pipe);
VkResult kk_dgc_eager_graphics_pipeline(struct kk_device *device, struct kk_shader *shader);
bool kk_dgc_init(struct kk_device *dev);
void kk_dgc_finish(struct kk_device *dev);
void *kk_dgc_new_icb(struct kk_device *dev, uint32_t count, bool compute);
uint64_t kk_dgc_icb_id(void *icb);
void kk_dgc_encode(struct kk_device *dev, mtl_compute_encoder *enc,
                   uint64_t parameters, void *icb, uint32_t count,
                   uint32_t phase);
void kk_dgc_execute_compute(mtl_compute_encoder *enc, void *icb, uint64_t range,
                            mtl_compute_pipeline_state *pipeline);
void kk_dgc_execute_graphics(mtl_render_encoder *enc, void *icb,
                             uint64_t range);
uint64_t kk_dgc_offset_count(struct kk_cmd_buffer *cmd, uint64_t address,
                             uint32_t first);
bool kk_dgc_draw_indirect_count(struct kk_cmd_buffer *cmd,
                                const VkDrawIndirectCount2InfoKHR *info,
                                bool indexed);
bool kk_dgc_needs_poly(struct kk_cmd_buffer *cmd);
void kk_dgc_draw_sequence(struct kk_cmd_buffer *cmd, uint64_t root,
                          uint64_t args, uint64_t binding, bool indexed, uint32_t draw_id);
void kk_dgc_set_linked_table_buffers(struct kk_cmd_buffer *cmd,
   uint64_t vertex_buffer, uint64_t fragment_buffer);
/* Records one GPU-generated sequence through the shared producer and retained
 * raster-reader boundary; returns only after every reader has been encoded. */
void kk_dgc_execute_layered_sequence(struct kk_cmd_buffer *cmd, uint64_t root,
   uint64_t args, uint64_t binding, bool indexed, uint32_t draw_id);
void kk_dgc_release_commands(struct kk_cmd_buffer *cmd);
uint32_t kk_dgc_primitive(struct kk_cmd_buffer *cmd);
bool kk_dgc_prepare_graphics_root(struct kk_cmd_buffer *allocator, struct kk_cmd_buffer *state, struct kk_root_descriptor_table *root);
void kk_dgc_flush_graphics(struct kk_cmd_buffer *cmd);
#endif
