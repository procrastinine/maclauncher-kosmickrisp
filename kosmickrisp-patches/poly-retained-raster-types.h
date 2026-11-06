/* SPDX-License-Identifier: MIT */
#ifndef KK_POLY_RETAINED_RASTER_TYPES_H
#define KK_POLY_RETAINED_RASTER_TYPES_H
#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#include "kosmickrisp/bridge/mtl_types.h"
#include "kosmickrisp/libkk/kk_poly_retained.h"
struct kk_cmd_buffer;
struct kk_draw_command;
struct kk_shader;
struct kk_shader_info;
struct nir_shader;
struct vk_graphics_pipeline_state;
struct kk_poly_retained_lifetime;

/* One prepared original draw, for one original ViewIndex and attachment slice.
 * The descriptor contains only the retained raster reader, never user stages.
 * slot_count_address is a GPU upper bound including empty/cull/stream slots.
 * A slot is an assembled original raster primitive, not an expanded triangle
 * and not the application-provided PrimitiveID varying. */
struct kk_poly_retained_raster {
   struct kk_cmd_buffer *command;
   struct kk_shader *producer;
   struct kk_poly_retained_lifetime *lifetime;
   mtl_argument_table *vertex_table;
   mtl_render_pipeline_descriptor *descriptor;
   uint64_t geometry_params, capture, slot_count_address, complete_draw;
   uint64_t per_draw_address, layer_selector_address;
   enum mtl_primitive_type primitive_type;
   enum mtl_primitive_topology_class topology;
   uint32_t view_mask, logical_layer;
   bool conservative;
};

/* Called synchronously at the existing final raster boundary, after original
 * stages/XFB/queries/capture. Encode every phase before returning. The callback
 * may build GPU range jobs before opening its live three-phase encoder; it
 * must not reset the heap, run producer stages, or retain this capture for a
 * later original draw. Command-pool records/BOs live through GPU retirement. */
typedef VkResult (*kk_poly_retained_consume)(struct kk_cmd_buffer *, void *,
   const struct kk_poly_retained_raster *);
struct kk_poly_retained_state {
   kk_poly_retained_consume consume;
   void *data;
   uint64_t per_draw_address, layer_selector_address;
   uint32_t logical_layer;
};
VkResult kk_poly_retained_begin(struct kk_cmd_buffer *, struct kk_draw_command *,
   kk_poly_retained_consume, void *);
void kk_poly_retained_end(struct kk_cmd_buffer *);

/* range_address names an immutable kk_poly_retained_range, produced by CPU
 * upload or GPU scheduling. result is a GPU address of four indirect-draw
 * words. No count readback or maximum primitive count is required. All native
 * subdivisions of each selected original primitive remain in the same job. */
VkResult kk_poly_retained_prepare_range(const struct kk_poly_retained_raster *,
   uint64_t range_address, uint64_t *result);
/* Pipeline is selected by the phase owner. This binds the original table only
 * to Vertex and the supplied stage-private table only to Fragment, then draws
 * on the supplied LIVE encoder. Reuse the same job for coverage/body/replay.
 * The original table must not be mutated by the phase consumer. */
VkResult kk_poly_retained_draw(const struct kk_poly_retained_raster *,
   mtl_render_encoder *, mtl_argument_table *fragment, uint64_t indirect_draw);

/* Apply the existing captured-reader input convention to a compilation clone
 * of the linked FS, before final IO/descriptor lowering. No new varying map. */
void kk_poly_retained_lower_fragment(struct nir_shader *,
   const struct kk_shader_info *, const struct vk_graphics_pipeline_state *);
mtl_render_pipeline_descriptor *kk_poly_retained_pipeline_descriptor(
   const struct kk_shader *, bool conservative);
#endif
