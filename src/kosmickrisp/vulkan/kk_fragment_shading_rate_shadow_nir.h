/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_SHADOW_NIR_H
#define KK_FRAGMENT_SHADING_RATE_SHADOW_NIR_H
#include "kk_interlock_load_nir.h"
#include "kk_fragment_shading_rate_clear_nir.h"

/* These are the existing shared attachment-load, replacement-replay and tile
 * vertex programs. Application output lowering and DS tests already execute
 * in the coarse transaction; none of these programs calls that application.
 * The phase owner supplies actual native views and real encoder ordering. */
static nir_shader *
kk_vrs_shadow_shader(enum kk_vrs_fragment_kind kind,
   const nir_shader_compiler_options *options,
   const struct vk_render_pass_state *rp,
   const struct vk_color_attachment_location_state *cal)
{
   struct kk_interlock_render_nir render = {.parameters = kk_vrs_cells_parameters};
   struct kk_interlock_replay_info info = {
      .depth = rp->depth_attachment_format != VK_FORMAT_UNDEFINED,
      .stencil = rp->stencil_attachment_format != VK_FORMAT_UNDEFINED,
   };
   for (unsigned physical = 0; physical < rp->color_attachment_count; physical++) {
      unsigned logical = cal->color_map[physical];
      if (logical != MESA_VK_ATTACHMENT_UNUSED)
         info.color_format[logical] = vk_format_to_pipe_format(rp->color_attachment_formats[physical]);
   }
   switch (kind) {
   case KK_VRS_CAPTURE_CLEAR:
      return kk_vrs_clear_shader(options);
   case KK_VRS_SHADOW_LOAD:
      return kk_interlock_load_shader(options, &render, &info, true);
   case KK_VRS_SHADOW_REPLAY:
      return kk_interlock_replay_shader(options, &render, &info);
   case KK_VRS_TILE_VERTEX:
      return kk_interlock_tile_vertex_shader(options);
   default:
      return NULL;
   }
}
#endif
