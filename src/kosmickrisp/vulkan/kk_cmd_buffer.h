/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_CMD_BUFFER_H
#define KK_CMD_BUFFER_H 1
#include "gallium/include/pipe/p_defines.h"
#include "poly/geometry.h"

#include "kk_private.h"

#include "kk_descriptor_set.h"
#include "kk_image.h"
#include "kk_nir_lower_vbo.h"
#include "kk_shader.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "kosmickrisp/libkk/kk_query.h"

#include "util/u_dynarray.h"

#include "vk_command_buffer.h"

#include <stdio.h>
#include <string.h>

struct kk_query_pool;

struct kk_root_descriptor_table {
   uint64_t addr;

   union {
      struct {
         /* Vertex input state */
         uint32_t buffer_strides[KK_MAX_VBUFS];
         uint64_t attrib_base[KK_MAX_ATTRIBS];
         uint32_t attrib_clamps[KK_MAX_ATTRIBS];
         uint64_t vertex_input;
         uint32_t sample_mask;
         uint32_t alpha_to_coverage;

         uint64_t fragment_statistic;
         float blend_constant[4];
         float clip_z_coeff;

         float viewport_z_range[KK_MAX_VIEWPORTS * 2];
         bool emulate_depth_clamp;
         bool emulate_viewport_z;
      } draw;
      struct {
         uint32_t base_group[3];
         /* Coordinates, not a lane map: a native load owns distribution. */
         uint16_t matrix_coordinates[64];
      } cs;
   };

   /* Descriptor set base addresses */
   union {
      uint64_t sets[KK_MAX_SETS];
      struct {
         uint64_t heap_resource, heap_sampler;
         uint64_t heap_embedded_samplers[MESA_ALL_SHADER_STAGES];
      };
   };

   /* Dynamic buffer bindings */
   struct kk_buffer_address dynamic_buffers[KK_MAX_DYNAMIC_BUFFERS];

   /* Start index in dynamic_buffers where each set starts */
   uint8_t set_dynamic_buffer_start[KK_MAX_SETS];

   uint64_t sparse_state;

   /* Client push constants: upload only the bound pipeline range. */
   uint8_t push[KK_MAX_PUSH_SIZE];
};

static_assert(2u + MESA_ALL_SHADER_STAGES <= KK_MAX_SETS,
              "embedded stage bindings fit the existing heap root union");
static_assert(sizeof(((struct kk_root_descriptor_table *)0)->sets) ==
                 KK_MAX_SETS * sizeof(uint64_t), "unchanged root union extent");

struct kk_descriptor_state {
   bool root_dirty;
   uint32_t push_size;
   struct kk_root_descriptor_table root;
   uint64_t heap_resource, heap_sampler;
   bool descriptor_heap;
   uint64_t heap_embedded_samplers[MESA_ALL_SHADER_STAGES];

   uint32_t set_sizes[KK_MAX_SETS];
   struct kk_descriptor_set *sets[KK_MAX_SETS];

   uint32_t push_dirty;
   struct kk_push_descriptor_set *push[KK_MAX_SETS];
};

/* Set bindings stay in CPU state. Heap aliases belong only to a snapshot. */
static inline void
kk_descriptor_root_apply_heap(const struct kk_descriptor_state *desc,
                              struct kk_root_descriptor_table *snapshot)
{
   if (desc->descriptor_heap) {
      snapshot->heap_resource = desc->heap_resource;
      snapshot->heap_sampler = desc->heap_sampler;
      memcpy(snapshot->heap_embedded_samplers, desc->heap_embedded_samplers,
             sizeof(snapshot->heap_embedded_samplers));
   }
}

static inline void
kk_descriptor_push_update(struct kk_descriptor_state *desc, uint32_t offset,
                           const void *data, size_t size)
{
   assert(offset <= KK_MAX_PUSH_SIZE && size <= KK_MAX_PUSH_SIZE - offset);
   if (size && memcmp(desc->root.push + offset, data, size)) {
      memcpy(desc->root.push + offset, data, size);
      desc->root_dirty = true;
   }
}

struct kk_per_draw_data {
   uint32_t draw_id;
   uint32_t index_size;
   /* Mask of outputs flowing VS->TCS, VS->GS, or TES->GS . */
   uint64_t vertex_outputs;

   /* Address of vertex param buffer if geom/tess is used, else 0 */
   uint64_t vertex_params;

   /* Address of tessellation param buffer if tessellation used, else 0 */
   uint64_t tess_params;

   uint64_t base_vertex_addr;
   uint64_t base_instance_addr;

   /* Address of geometry param buffer if a GS is used, else 0 */
   uint64_t geometry_params;
   uint64_t geometry_statistics[PIPE_STAT_QUERY_COUNT];

   /* Whether the last vertex of a primitive is its provoking vertex */
   uint32_t provoking_last;
   uint32_t rasterization_stream;

   /* PerVertexKHR emulation. Index buffer of the draw, 0 if non-indexed. */
   uint64_t per_vertex_index_addr;
   uint32_t per_vertex_index_size;
   uint32_t per_vertex_first_vertex;
   int32_t per_vertex_base_vertex;
   uint32_t per_vertex_topology;
   /* Number of index buffer elements. */
   uint32_t per_vertex_index_count;
   /* The original indirect command is drawn, so vertex IDs start at its first
    * vertex/index plus vertex offset. */
   uint32_t per_vertex_raw_indirect;
   /* Original indirect command of the draw, 0 for direct draws. */
   uint64_t per_vertex_indirect_addr;
   /* Heap the PerVertexKHR buffer is allocated from, 0 if unused. */
   uint64_t per_vertex_heap;
   /* Optional Poly multiview tail. Ordinary draws do not upload it. */
   uint64_t multiview_index;
};

struct kk_depth_stencil_record;

struct kk_attachment {
   VkRenderingAttachmentFlagsKHR flags;
   VkFormat vk_format;
   struct kk_image_view *iview;

   VkResolveModeFlagBits resolve_mode;
   struct kk_image_view *resolve_iview;

   VkImageLayout image_layout;
   VkAttachmentLoadOp load_op;
   VkAttachmentStoreOp store_op;
   VkClearValue clear_value;
};

struct kk_rendering_state {
   bool layered_bc;
   uint32_t layered_bc_layer;
   uint32_t layered_bc_view_mask;
   bool attachmentless;
   bool msrtss;
   VkRenderingFlagBits flags;

   VkRect2D area;
   uint32_t layer_count;
   uint32_t view_mask;
   uint32_t samples;

   uint32_t color_att_count;
   struct kk_attachment color_att[KK_MAX_RTS];
   uint8_t color_map[KK_MAX_RTS];
   struct kk_attachment depth_att;
   struct kk_attachment stencil_att;
   struct kk_attachment fsr_att;

   bool ms_bresenham_lines;
   /* Barrier tracking to understand if we need to split render passes. */
   bool write_available;
   bool ds_write_available;
   bool storage_write_available;
   bool sample_locations_enable;
   uint32_t sample_locations_count;
   VkSampleLocationEXT sample_locations[KK_MAX_SAMPLES];
   bool force_attachment_store;
   bool writable_msaa_loaded;
   bool rendering_to_whole_framebuffer;
};

/* Dirty tracking bits for state not tracked by vk_dynamic_graphics_state or
 * shaders_dirty.
 */
enum kk_dirty {
   KK_DIRTY_VB = BITFIELD_BIT(0),
   KK_DIRTY_OCCLUSION = BITFIELD_BIT(1),
};

#include "kk_poly_layer_replay_types.h"
#include "kk_poly_retained_raster_types.h"

struct kk_graphics_state {
   struct kk_poly_layer_replay_state layer_replay;
   struct kk_poly_retained_state poly_retained;
   struct kk_shader *shader;
   uint64_t mesh_primitives_query;
   uint64_t mesh_payloads, mesh_records, mesh_batch, mesh_slices;
   uint64_t mesh_native_batch, mesh_native_slices;
   uint64_t mesh_retained_outputs, mesh_retained_output_bytes;
   uint64_t mesh_per_draw_address;
   struct kk_mesh_retained_consumer mesh_retained_consumer;
   bool polygon_points;
   bool conservative;
   uint64_t polygon_capture;
   struct kk_ptr interlock_capture;
   uint32_t interlock_source_view;
   uint32_t interlock_source_view_ordinal;
   struct kk_rendering_state render;
   struct kk_descriptor_state descriptors;
   struct kk_per_draw_data per_draw_data;
   uint32_t indirect_draw_id;
   bool dgc_icb;
   uint64_t dgc_linked_vs_buffer, dgc_linked_fs_buffer;
   uint64_t generated_root;
   uint64_t generated_heap_floor;
   struct kk_addr_range xfb[POLY_MAX_SO_BUFFERS];
   uint64_t xfb_offsets;
   uint64_t xfb_query[POLY_MAX_VERTEX_STREAMS];
   uint64_t primitives_query[POLY_MAX_VERTEX_STREAMS];
   bool xfb_enabled;

   mtl_depth_stencil_state *depth_stencil_state;
   mtl_render_pass_descriptor *render_pass_descriptor;
   bool is_depth_stencil_dynamic;
   bool is_cull_front_and_back;
   bool need_to_start_render_pass;

   enum kk_dirty dirty;
   uint32_t pipeline_sample_count;

   struct {
      enum mtl_visibility_result_mode mode;

      /* If enabled, index of the current occlusion query in the occlusion heap.
       * There can only be one active at a time (hardware constraint).
       */
      uint16_t index;
   } occlusion;

   /* Index buffer */
   struct {
      struct kk_addr_range gpu;
      uint8_t bytes_per_index;
      uint64_t null_addr;
      /* uint32 0, 1, 2, ... for PerVertexKHR emulated indirect draws. */
      struct kk_addr_range identity;
   } index;

   /* Vertex buffers */
   struct {
      struct kk_addr_range addr_range[KK_MAX_VBUFS];
   } vb;

   /* Tessellation state */
   struct {
      /* Grid buffer for when the draw is indirect */
      struct kk_ptr indirect_ptr;
      uint64_t out_draws_addr;
      struct kk_tess_info info;
      enum mesa_prim prim;
   } tess;

   /* Rasterization draw of the GS output */
   struct {
      struct kk_addr_range index;
      uint32_t index_count;
      uint32_t instance_count;
   } gs;

   /* Needed by vk_command_buffer::dynamic_graphics_state */
   struct vk_vertex_input_state _dynamic_vi;
   struct vk_sample_locations_state _dynamic_sl;
};

struct kk_compute_state {
   struct kk_descriptor_state descriptors;
   mtl_compute_pipeline_state *bound_pipeline;
};

struct kk_conditional_rendering_state {
   uint64_t address;
   bool inverted;
   bool enabled;
};

struct kk_encoder;

struct kk_uploader {
   /** List of kk_cmd_bo */
   struct list_head bos;

   /* Current addresses */
   struct kk_bo *bo;
   uint32_t offset;
};

/* A pending resolve of one timestamp counter-heap entry into a query pool BO.
 * Recorded by vkCmdWriteTimestamp2 and flushed on the GPU timeline at cs_end. */
struct kk_ts_resolve {
   mtl_counter_heap *heap;
   uint32_t index;
   uint64_t dst_addr;
};

struct kk_ts_stage_entry {
   const struct kk_query_pool *pool;
   mtl_counter_heap *heap;
   enum mtl_render_stages stage;
   uint32_t index;
};

struct kk_msrtss_image {
   VkImage image;
   VkDeviceMemory memory;
   struct kk_image_view *source;
   VkImageView view;
   VkSampleCountFlagBits samples;
};

struct kk_whole_pass_state {
   bool suspended;
   VkRect2D area;
   uint32_t layers, view_mask, color_count;
   VkImageView colors[KK_MAX_RTS], depth, stencil;
   VkSampleCountFlagBits implicit_samples;
};

struct kk_cmd_buffer {
   struct vk_command_buffer vk;
   struct kk_depth_stencil_record *depth_stencil_records;
   struct kk_interlock_segment *interlock_segments;
   struct kk_interlock_segment *interlock_segments_tail;
   struct kk_interlock_heap *interlock_heap;
   bool interlock_heap_active;
   bool interlock_heap_used;
   struct kk_whole_pass_state whole_pass;
   bool deferred_render_chain;
   bool transient_safe;

   struct {
      mtl_command_allocator *allocator;
      mtl_command_buffer *cmd_buf;
      mtl_render_encoder *render;
      mtl_compute_encoder *compute;
   } metal;
   struct util_dynarray mesh_command_buffers;
   struct util_dynarray mesh_allocators;
   uint32_t mesh_batches_in_part, mesh_draws_in_part;
   /* Pending timestamp resolves (struct kk_ts_resolve), flushed at cs_end. */
   bool writable_msaa_meta;
   uint32_t writable_msaa_render_attachments;
   uint32_t writable_msaa_render_writes;
   struct util_dynarray msrtss_images;
   struct util_dynarray ts_resolves;
   /* Timestamps already sampled by the current render encoder. (struct
    * kk_ts_stage_entry). */
   struct util_dynarray ts_stage_map;
   /* Deferred writes due to being mid render encoder */
   struct util_dynarray post_render_writes;

   void *drawable;
   struct util_dynarray submit_drawables;
   mtl_argument_table *argument_table;
   mtl_argument_table *mesh_table;

   struct {
      struct kk_graphics_state gfx;
      struct kk_compute_state cs;
      uint64_t compute_statistic;
      uint64_t pipeline_statistics;
      VkQueryPipelineStatisticFlags pipeline_statistics_flags;
      struct kk_conditional_rendering_state cond_render;
      struct kk_shader *shaders[MESA_ALL_SHADER_STAGES];
      /* Address of the binding 0 for when compute dispatches modify it.
       * We are trying to be nice to ourselves. */
      uint64_t root_addr;
      mtl_argument_table *root_table;
      /* Only tracks graphics shaders since compute is always bound for now. */
      uint32_t dirty_shaders;
   } state;

   struct kk_uploader uploader;

   /* Owned large BOs */
   struct util_dynarray large_bos;
   struct util_dynarray dgc_commands;

   /* Does the command buffer use the geometry heap? */
   bool uses_heap;
   /* Set at vkBeginCommandBuffer. One-time-submit buffers skip command
    * enqueueing in the trampolines since they can never be replayed. */
   bool enqueue_commands;
   bool one_time_submit;
   bool sparse_guarded;
   /* Metal command buffers are single-shot: a resubmission must re-record
    * by replaying the enqueued commands (see rerecord_cmd_buffer). */
   bool submitted;
};

VK_DEFINE_HANDLE_CASTS(kk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

extern const struct vk_command_buffer_ops kk_cmd_buffer_ops;

static inline struct kk_device *
kk_cmd_buffer_device(struct kk_cmd_buffer *cmd)
{
   return (struct kk_device *)cmd->vk.base.device;
}

static inline struct kk_cmd_pool *
kk_cmd_buffer_pool(struct kk_cmd_buffer *cmd)
{
   return (struct kk_cmd_pool *)cmd->vk.pool;
}

static inline struct kk_descriptor_state *
kk_get_descriptors_state(struct kk_cmd_buffer *cmd,
                         VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      return &cmd->state.gfx.descriptors;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmd->state.cs.descriptors;
   default:
      UNREACHABLE("Unhandled bind point");
   }
};

void cs_start_render(struct kk_cmd_buffer *cmd);
mtl_render_encoder *cs_get_render(struct kk_cmd_buffer *cmd);
mtl_compute_encoder *cs_get_compute(struct kk_cmd_buffer *cmd);
mtl_compute_encoder *cs_get_compute_for_dispatch(struct kk_cmd_buffer *cmd);
void cs_end(struct kk_cmd_buffer *cmd);
bool kk_mesh_split_command_buffer(struct kk_cmd_buffer *cmd);
void kk_cmd_bind_root_to_argument_table(struct kk_cmd_buffer *cmd,
                                        uint64_t addr);

static void
kk_cmd_buffer_dirty_all_gfx(struct kk_cmd_buffer *cmd)
{
   /* Ensure we flush all graphics state */
   vk_dynamic_graphics_state_dirty_all(&cmd->vk.dynamic_graphics_state);
   cmd->state.dirty_shaders = ~0u;
   cmd->state.gfx.dirty = ~0u;
   cmd->state.gfx.descriptors.root_dirty = true;

   /* We just flushed out the heap use. If we want to use it again, we'll need
    * to queue a free for it again.
    */
   cmd->uses_heap = false;
}

void kk_cmd_release_dynamic_ds_state(struct kk_cmd_buffer *cmd);

mtl_depth_stencil_state *
kk_compile_depth_stencil_state(struct kk_device *device,
                               const struct vk_depth_stencil_state *ds,
                               bool has_depth, bool has_stencil);

void kk_meta_expand_msrtss(struct kk_cmd_buffer *cmd, struct kk_image_view *src,
                           VkImage dst, VkImageAspectFlags aspects);

void kk_meta_resolve_rendering(struct kk_cmd_buffer *cmd,
                               const VkRenderingInfo *pRenderingInfo);

struct kk_ptr kk_pool_alloc(struct kk_cmd_buffer *cmd, uint32_t size,
                            uint32_t alignment);

/* Shader eligibility is compiled once. Refresh the selector at bind or on a
 * conservative-mode state change, never by re-evaluating every draw. */
static inline void
kk_cmd_buffer_update_conservative(struct kk_cmd_buffer *cmd,
                                 const struct kk_shader *shader)
{
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   bool active = shader->info.conservative && dyn->rs.conservative_mode ==
      VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
   if (active != cmd->state.gfx.conservative) {
      cmd->state.gfx.conservative = active;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
   }
}

struct kk_ptr kk_pool_upload(struct kk_cmd_buffer *cmd, const void *data,
                             uint32_t size, uint32_t alignment);

uint64_t kk_upload_descriptor_root_data(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size);

uint64_t kk_upload_descriptor_root(struct kk_cmd_buffer *cmd,
                                   VkPipelineBindPoint bind_point);

void kk_cmd_buffer_flush_push_descriptors(struct kk_cmd_buffer *cmd,
                                          struct kk_descriptor_state *desc);

void kk_apply_attachment_store_ops(struct kk_cmd_buffer *cmd, bool force_store);

bool kk_attachment_do_renderpass_resolve(const struct kk_attachment *attachment,
                                         bool rendering_to_whole_framebuffer,
                                         VkImageAspectFlags aspect);

enum kk_grid_mode {
   KK_GRID_DIRECT = 0u,
   KK_GRID_INDIRECT,
   /* Thread counts, then threadgroup size, like poly's grids */
   KK_GRID_INDIRECT_LOCAL,
};
struct kk_grid {
   enum kk_grid_mode mode;
   union {
      struct mtl_size size;
      uint64_t addr;
   };
};

static struct kk_grid
kk_grid_3d(uint32_t x, uint32_t y, uint32_t z)
{
   return (struct kk_grid){
      .mode = KK_GRID_DIRECT,
      .size = {x, y, z},
   };
}

static struct kk_grid
kk_grid_2d(uint32_t x, uint32_t y)
{
   return kk_grid_3d(x, y, 1u);
}

static struct kk_grid
kk_grid_1d(uint32_t x)
{
   return kk_grid_3d(x, 1u, 1u);
}

static struct kk_grid
kk_grid_indirect(uint64_t addr)
{
   return (struct kk_grid){
      .mode = KK_GRID_INDIRECT,
      .addr = addr,
   };
}

static struct kk_grid
kk_grid_indirect_local(uint64_t addr)
{
   return (struct kk_grid){
      .mode = KK_GRID_INDIRECT_LOCAL,
      .addr = addr,
   };
}

static bool
kk_grid_is_indirect(struct kk_grid grid)
{
   return grid.mode != KK_GRID_DIRECT;
}

void kk_dispatch_precomp(struct kk_cmd_buffer *cmd, struct kk_grid grid,
                         bool pre_gfx, enum libkk_program idx, void *data,
                         size_t data_size);

#define MESA_DISPATCH_PRECOMP kk_dispatch_precomp

void kk_cmd_write(struct kk_cmd_buffer *cmd, struct libkk_imm_write write);

void kk_cmd_buffer_set_label(struct kk_cmd_buffer *cb, const char *label);


bool kk_whole_pass_transient_safe(struct kk_device *, struct vk_command_buffer **, uint32_t);
bool kk_whole_pass_needs_submit(struct kk_cmd_buffer *cmd);
void kk_whole_pass_command_boundary(struct kk_cmd_buffer *cmd);
void kk_whole_pass_flush_suspended(struct kk_cmd_buffer *cmd);
VkAccessFlags2 kk_whole_pass_attachment_access(VkAccessFlags2, VkPipelineStageFlags2);
bool kk_whole_pass_attachment_barrier(const VkDependencyInfo *dep);
bool kk_whole_pass_local_barrier(const VkDependencyInfo *dep);
void kk_init_rendering_locations(struct kk_cmd_buffer *cmd, const VkRenderingInfo *info);
VKAPI_ATTR void VKAPI_CALL kk_whole_pass_begin_rendering(VkCommandBuffer, const VkRenderingInfo *);
VKAPI_ATTR void VKAPI_CALL kk_whole_pass_end_rendering(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL kk_whole_pass_end_rendering2(VkCommandBuffer, const VkRenderingEndInfoKHR *);

#endif
