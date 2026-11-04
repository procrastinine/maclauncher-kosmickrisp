/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_IMAGE_VIEW_H
#define KK_IMAGE_VIEW_H 1

#include "kk_private.h"
#include "kk_image.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "util/format/u_formats.h"

#include "vk_image.h"

struct kk_device;
struct kk_sparse_view_state;

struct kk_image_view {
   struct vk_image_view vk;
   struct kk_layered_bc_view *layered_bc;
   struct kk_writable_msaa_view *writable_msaa;
   struct kk_sparse_view_state *sparse;

   uint8_t plane_count;
   struct {
      uint8_t image_plane;

      enum pipe_format format;

      mtl_texture *mtl_handle_sampled;
      mtl_texture
         *mtl_handle_storage; // TODO_KOSMICKRISP We can probably get rid of
                              // this once we lower 2D cubes and 3D to 2D array?

      /* Cached handle so we don't have to retrieve it from the image when we
       * render */
      mtl_texture *mtl_handle_render;
      mtl_texture *transient_private;
      bool render_is_view;

      /* Input attachment handle. Required since input attachments needs to be
       * arrays, and sampled may not be */
      mtl_texture *mtl_handle_input;

      /* Used if view has to create its own alias of a specific subresource */
      mtl_texture *mtl_handle_subres;

      /* Cube face views and M11 aliases are disjoint native view types.
       * Only a single-plane 1D/2D view publishes the M11 member. Both usages
       * share its identical native view, cache and retirement reference. */
      union {
         mtl_texture *mtl_handle_cube_face;
         mtl_texture *mtl_handle_single_layer_alias;
      };
      union {
         uint64_t cube_face_gpu_resource_id;
         uint64_t single_layer_alias_gpu_resource_id;
      };
      uint64_t sampled_gpu_resource_id;
      uint64_t storage_gpu_resource_id;
      uint64_t input_gpu_resource_id;
   } planes[3];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(kk_image_view, vk.base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW)

static inline mtl_texture *
kk_transient_render_texture(const struct kk_image_view *view)
{
   const struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   if (!image->planes[0].memoryless)
      return view->planes[0].mtl_handle_render;
   return kk_transient_memoryless_render_texture(view);
}

VkResult kk_image_view_init(struct kk_device *dev, struct kk_image_view *view,
                            const VkImageViewCreateInfo *pCreateInfo);

void kk_image_view_finish(struct kk_device *dev, struct kk_image_view *view);

void kk_image_view_set_label(struct kk_image_view *iview, const char *label);

#endif /* KK_IMAGE_VIEW_H */
