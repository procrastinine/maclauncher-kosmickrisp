/* SPDX-License-Identifier: MIT */
#ifndef KK_COMMAND_DEPTH_STENCIL_STATE_H
#define KK_COMMAND_DEPTH_STENCIL_STATE_H

#include "kk_cmd_buffer.h"

struct kk_image;
struct kk_shader;

/* ORIGINAL is the selected committing state; READ_ONLY keeps its tests but
 * disables every write; BYPASS disables tests, writes and depth bounds.
 * Selecting a mode never executes a test, draw, barrier or encoder transition. */
enum kk_depth_stencil_mode {
   KK_DEPTH_STENCIL_ORIGINAL,
   KK_DEPTH_STENCIL_READ_ONLY,
   KK_DEPTH_STENCIL_BYPASS,
};

struct kk_depth_stencil_snapshot {
   struct kk_cmd_buffer *command;
   struct kk_shader *producer;
   mtl_render_encoder *encoder;
   mtl_depth_stencil_state *selected;
   /* Complete bound Vulkan state, including both references and bounds. */
   struct vk_depth_stencil_state ds;
   struct kk_rendering_state render;
   const struct kk_image *depth_image, *stencil_image;
   mtl_texture *depth_texture, *stencil_texture;
   VkSampleCountFlagBits rasterization_samples;
   VkImageAspectFlags present_aspects, tested_aspects;
   VkImageAspectFlags read_only_aspects, memoryless_aspects;
   uint32_t memoryless_color_mask;
   bool dynamic;
   bool native_depth_bounds;
};

/* Shared with ordinary pipeline binding. No allocation or encoder creation. */
static inline mtl_depth_stencil_state *
kk_cmd_selected_depth_stencil(struct kk_cmd_buffer *cmd,
                             struct kk_shader *actual_producer)
{
   return cmd->state.gfx.is_depth_stencil_dynamic
      ? cmd->state.gfx.depth_stencil_state
      : kk_attachment_depth_stencil(cmd, actual_producer);
}

/* Call after graphics-state flush for the actual producer and current render
 * encoder, before an internal coverage/body/replay sequence. The immutable CPU
 * snapshot and retained handles belong to cmd until reset/destroy after GPU
 * completion. Vulkan views/images/producer keep their existing API lifetime.
 * No attachment load/store or encoder transition is performed here. */
VkResult
kk_cmd_capture_depth_stencil(struct kk_cmd_buffer *cmd,
                            struct kk_shader *actual_producer,
                            const struct kk_depth_stencil_snapshot **out);

/* Derived native states are compiled lazily through the existing compiler.
 * BYPASS is shared by the command; READ_ONLY belongs to its snapshot. Both
 * remain retained until command retirement. Never release a returned handle. */
mtl_depth_stencil_state *
kk_cmd_depth_stencil_variant(const struct kk_depth_stencil_snapshot *snapshot,
                            enum kk_depth_stencil_mode mode);

/* Use in the SAME live encoder. Restores both stencil references and bounds,
 * including disabled [0,1] bounds; BYPASS always disables bounds. This binds
 * state only: the consumer chooses exactly one ORIGINAL committing phase. */
VkResult
kk_cmd_bind_depth_stencil(const struct kk_depth_stencil_snapshot *snapshot,
                          enum kk_depth_stencil_mode mode);

/* Command reset/destroy, after recorded native storage is retired. */
void kk_cmd_depth_stencil_finish(struct kk_cmd_buffer *cmd);

#endif
