/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_image_view.h"

#include "util/u_atomic.h"
#include "kk_layered_bc.h"
#include "kk-writable-msaa.h"

#include "kk_device.h"
#include "kk_perf.h"
#include "kk_entrypoints.h"
#include "kk_format.h"
#include "kk_image.h"
#include "kk_sparse.h"
#include "kk_physical_device.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_format.h"

#include "vk_format.h"
#define KK_MAINTENANCE11_VIEW
#include "kk_maintenance11.h"

static enum pipe_swizzle
vk_swizzle_to_pipe(VkComponentSwizzle swizzle)
{
   switch (swizzle) {
   case VK_COMPONENT_SWIZZLE_R:
      return PIPE_SWIZZLE_X;
   case VK_COMPONENT_SWIZZLE_G:
      return PIPE_SWIZZLE_Y;
   case VK_COMPONENT_SWIZZLE_B:
      return PIPE_SWIZZLE_Z;
   case VK_COMPONENT_SWIZZLE_A:
      return PIPE_SWIZZLE_W;
   case VK_COMPONENT_SWIZZLE_ONE:
      return PIPE_SWIZZLE_1;
   case VK_COMPONENT_SWIZZLE_ZERO:
      return PIPE_SWIZZLE_0;
   default:
      UNREACHABLE("Invalid component swizzle");
   }
}

static enum VkImageViewType
remove_1d_view_types(enum VkImageViewType type)
{
   if (type == VK_IMAGE_VIEW_TYPE_1D)
      return VK_IMAGE_VIEW_TYPE_2D;
   if (type == VK_IMAGE_VIEW_TYPE_1D_ARRAY)
      return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   return type;
}

/* Native allocation/view failure must not become a successful descriptor with
 * a zero resource ID in release builds. Validate once at view construction. */
static const char *
kk_image_view_plane_error(const struct kk_device *dev,
                          const struct kk_image_view *view, unsigned p)
{
   if ((view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
       (!view->planes[p].mtl_handle_sampled ||
        !view->planes[p].sampled_gpu_resource_id))
      return "sampled";
   if ((view->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
       (!view->planes[p].mtl_handle_storage ||
        !view->planes[p].storage_gpu_resource_id))
      return "storage";
   if ((view->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
       (!view->planes[p].mtl_handle_input ||
        !view->planes[p].input_gpu_resource_id))
      return "input attachment";
   if ((view->vk.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
       !view->planes[p].mtl_handle_render)
      return "render attachment";
   if ((view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
       dev->vk.enabled_features.nonSeamlessCubeMap &&
       (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
        view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
       (!view->planes[p].mtl_handle_cube_face ||
        !view->planes[p].cube_face_gpu_resource_id))
      return "cube face";
   return NULL;
}

/* The opaque WSI source is stable across acquisitions. The drawable-backed
 * destination is rebound on each acquisition and must never enter this cache.
 * This one canonical view needs no general per-image view map or mutex. */
static mtl_texture *
kk_sampled_image_view(struct kk_device *dev, struct kk_image *image,
                      const struct kk_image_view *view,
                      VkImageViewCreateFlags flags, mtl_texture *texture,
                      const struct kk_view_layout *layout)
{
   const bool cacheable = image->wsi_opaque_alpha &&
      (flags & VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA) &&
      image->plane_count == 1 && view->plane_count == 1 &&
      image->vk.image_type == VK_IMAGE_TYPE_2D &&
      image->vk.mip_levels == 1 && image->vk.array_layers == 1 &&
      image->vk.samples == VK_SAMPLE_COUNT_1_BIT &&
      view->vk.aspects == VK_IMAGE_ASPECT_COLOR_BIT &&
      view->vk.usage == VK_IMAGE_USAGE_SAMPLED_BIT &&
      view->vk.format == image->vk.format &&
      view->vk.swizzle.r == VK_COMPONENT_SWIZZLE_R &&
      view->vk.swizzle.g == VK_COMPONENT_SWIZZLE_G &&
      view->vk.swizzle.b == VK_COMPONENT_SWIZZLE_B &&
      view->vk.swizzle.a == VK_COMPONENT_SWIZZLE_ONE &&
      layout->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
      layout->base_level == 0 && layout->num_levels == 1 &&
      layout->base_array_layer == 0 && layout->array_len == 1 &&
      layout->min_lod_clamp == 0.0f &&
      layout->format.mtl == image->planes[0].layout.format.mtl &&
      !image->sparse && !image->planes[0].memoryless &&
      texture == image->planes[0].mtl_handle;
   if (cacheable) {
      mtl_texture *cached =
         __atomic_load_n(&image->wsi_opaque_sampled_view, __ATOMIC_ACQUIRE);
      if (cached)
         return mtl_retain(cached);
   }

   mtl_texture *created = KK_PERF_CALL(dev, KK_PERF_SAMPLED_VIEW_CREATE,
      mtl_new_texture_view_with(texture, layout));
   if (!created || !cacheable)
      return created;

   /* The image keeps the creation reference. Every VkImageView keeps its own
    * reference, including concurrent first-use callers. Failed construction
    * leaves the slot empty and follows ordinary view error propagation. */
   mtl_texture *expected = NULL;
   if (__atomic_compare_exchange_n(&image->wsi_opaque_sampled_view, &expected,
                                  created, false, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE))
      return mtl_retain(created);

   mtl_release(created);
   return mtl_retain(expected);
}

VkResult
kk_image_view_init(struct kk_device *dev, struct kk_image_view *view,
                   const VkImageViewCreateInfo *pCreateInfo)
{
   VK_FROM_HANDLE(kk_image, image, pCreateInfo->image);

   memset(view, 0, sizeof(*view));

   vk_image_view_init(&dev->vk, &view->vk, pCreateInfo);
   /* Shaders apply a minimum LOD only in their sampler-emulation
    * specialization, which draws select once the device has such a view. */
   if (view->vk.min_lod > view->vk.base_mip_level)
      p_atomic_set(&dev->sampler_emulation, 1);
   if (image->planes[0].memoryless &&
       !(pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA) &&
       !vk_find_struct_const(pCreateInfo->pNext, IMAGE_VIEW_USAGE_CREATE_INFO))
      view->vk.usage &= image->application_usage;

   if (image->wsi_opaque_alpha &&
       (pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA))
      view->vk.swizzle.a = VK_COMPONENT_SWIZZLE_ONE;

   /* First, figure out which image planes we need.
    * For depth/stencil, we only have plane so simply assert
    * and then map directly betweeen the image and view plane
    */
   if (image->vk.aspects &
       (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      assert(image->plane_count == 1);
      assert(kk_image_aspects_to_plane(image, view->vk.aspects) == 0);
      view->plane_count = 1;
      view->planes[0].image_plane = 0;
   } else {
      /* For other formats, retrieve the plane count from the aspect mask
       * and then walk through the aspect mask to map each image plane
       * to its corresponding view plane
       */
      assert(util_bitcount(view->vk.aspects) ==
             vk_format_get_plane_count(view->vk.format));
      view->plane_count = 0;
      u_foreach_bit(aspect_bit, view->vk.aspects) {
         uint8_t image_plane =
            kk_image_aspects_to_plane(image, 1u << aspect_bit);
         view->planes[view->plane_count++].image_plane = image_plane;
      }
   }
   const bool layered_bc =
      ((image->vk.create_flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) ||
       (pCreateInfo->flags & VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA)) &&
      kk_layered_bc_requested(&view->vk);

   /* Finally, fill in each view plane separately */
   for (unsigned view_plane = 0; view_plane < view->plane_count; view_plane++) {
      const uint8_t image_plane = view->planes[view_plane].image_plane;
      struct kk_image_plane *plane = &image->planes[image_plane];

      const struct vk_format_ycbcr_info *ycbcr_info =
         vk_format_get_ycbcr_info(view->vk.format);
      assert(ycbcr_info || view_plane == 0);
      VkFormat plane_format =
         ycbcr_info ? ycbcr_info->planes[view_plane].format : view->vk.format;
      enum pipe_format p_format = vk_format_to_pipe_format(plane_format);
      if (view->vk.aspects == VK_IMAGE_ASPECT_DEPTH_BIT)
         p_format = vk_format_to_pipe_format(image->vk.format);
      else if (view->vk.aspects == VK_IMAGE_ASPECT_STENCIL_BIT)
         p_format = util_format_stencil_only(
            vk_format_to_pipe_format(image->vk.format));

      view->planes[view_plane].format = p_format;
      const struct kk_va_format *supported_format = kk_get_va_format(p_format);
      assert(supported_format);

      struct kk_view_layout view_layout = {
         .view_type = remove_1d_view_types(view->vk.view_type),
         .sample_count_sa = plane->layout.sample_count_sa,
         .format = {.pipe = kk_format_backing(p_format),
                    .mtl = supported_format->mtl_pixel_format},
         .base_level = view->vk.base_mip_level,
         .num_levels = view->vk.level_count,
         .base_array_layer = view->vk.base_array_layer,
         .array_len = view->vk.layer_count,
         .min_lod_clamp = kk_device_physical(dev)->info.native_image_view_min_lod
            ? view->vk.min_lod : 0,
      };
      uint8_t view_swizzle[4] = {vk_swizzle_to_pipe(view->vk.swizzle.r),
                                 vk_swizzle_to_pipe(view->vk.swizzle.g),
                                 vk_swizzle_to_pipe(view->vk.swizzle.b),
                                 vk_swizzle_to_pipe(view->vk.swizzle.a)};
      util_format_compose_swizzles(supported_format->unswizzle.channels,
                                   view_swizzle, view_layout.swizzle.channels);

      /* When sampling a depth/stencil texture Metal returns (d, d, d, 1), but
       * Vulkan requires (d, 0, 0, 1). This means, we need to convert G and B to
       * 0 */
      if (util_format_is_depth_or_stencil(p_format)) {
         if (view_layout.swizzle.red == PIPE_SWIZZLE_Y ||
             view_layout.swizzle.red == PIPE_SWIZZLE_Z)
            view_layout.swizzle.red = PIPE_SWIZZLE_0;
         if (view_layout.swizzle.green == PIPE_SWIZZLE_Y ||
             view_layout.swizzle.green == PIPE_SWIZZLE_Z)
            view_layout.swizzle.green = PIPE_SWIZZLE_0;
         if (view_layout.swizzle.blue == PIPE_SWIZZLE_Y ||
             view_layout.swizzle.blue == PIPE_SWIZZLE_Z)
            view_layout.swizzle.blue = PIPE_SWIZZLE_0;
         if (view_layout.swizzle.alpha == PIPE_SWIZZLE_Y ||
             view_layout.swizzle.alpha == PIPE_SWIZZLE_Z)
            view_layout.swizzle.alpha = PIPE_SWIZZLE_0;
      }

      if (plane->memoryless && (kk_image_plane_is_committed(plane) ||
          (view->vk.usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)))) {
         VkResult result = kk_image_plane_require_private(dev, plane);
         if (result != VK_SUCCESS)
            return result;
      }
      mtl_texture *mtl_handle = kk_image_plane_render_base(plane);

      /* Block texel view requires subresource aliasing. We do it here to avoid
       * having to create a texture for every possible subresource up-front */
      if (util_format_is_compressed(plane->layout.format.pipe) &&
          !util_format_is_compressed(view_layout.format.pipe)) {
         if (image->sparse && image->sparse->standard_3d) {
            VkResult result=kk_standard_3d_raw_view(dev,view,&view_layout,&mtl_handle);
            if(result!=VK_SUCCESS){kk_image_view_finish(dev,view);return result;}
            view->planes[view_plane].mtl_handle_subres=mtl_handle;
            view_layout.base_level=view_layout.base_array_layer=0;
            view_layout.num_levels=view_layout.array_len=1;
         } else {
         uint64_t subres_offset_B =
            kk_get_layer_level_B(&plane->layout, view_layout.base_array_layer,
                                 view_layout.base_level);

         /* Construct layout for the reinterpreted image */
         struct kk_image_layout subres_layout = plane->layout;
         subres_layout.width_px = view->vk.extent.width;
         subres_layout.height_px = view->vk.extent.height;
         subres_layout.depth_px = view->vk.extent.depth;
         subres_layout.layers = layered_bc ? 1 : view->vk.layer_count;
         subres_layout.levels = view->vk.level_count;
         subres_layout.format.pipe = view_layout.format.pipe;
         subres_layout.format.mtl = view_layout.format.mtl;

         if (image->vk.image_type == VK_IMAGE_TYPE_3D &&
             view->vk.view_type != VK_IMAGE_VIEW_TYPE_3D) {
            subres_layout.type = MTL_TEXTURE_TYPE_2D_ARRAY;
            subres_layout.depth_px = 1u;
         }

         mtl_handle = kk_image_plane_create_texture(plane, &subres_layout,
                                                    subres_offset_B);
         view->planes[view_plane].mtl_handle_subres = mtl_handle;

         /* Adjust for new base texture pointing to subresource */
         view_layout.base_level = 0u;
         if (layered_bc)
            {
            view_layout.min_lod_clamp = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0f);
            view_layout.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
         }
         view_layout.num_levels = subres_layout.levels;
         view_layout.base_array_layer = 0u;
         view_layout.array_len = subres_layout.layers;
         }
      } else if (image->vk.image_type == VK_IMAGE_TYPE_3D &&
                 view->vk.view_type != VK_IMAGE_VIEW_TYPE_3D) {
         mtl_handle = image->planes[image_plane].mtl_handle_array;
      }

      if (view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
         VkResult result = kk_sparse_image_view_query(dev, view, mtl_handle, &view_layout, false);
         if (result != VK_SUCCESS) { kk_image_view_finish(dev, view); return result; }
         view->planes[view_plane].mtl_handle_sampled =
            KK_PERF_CALL(dev, KK_PERF_SAMPLED_VIEW_GET,
               kk_sampled_image_view(dev, image, view, pCreateInfo->flags,
                                      mtl_handle, &view_layout));
         view->planes[view_plane].sampled_gpu_resource_id =
            mtl_texture_get_gpu_resource_id(
               view->planes[view_plane].mtl_handle_sampled);
      }

      if ((view->vk.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
          dev->vk.enabled_features.nonSeamlessCubeMap &&
          (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
           view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
         struct kk_view_layout face_layout = view_layout;
         face_layout.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
         view->planes[view_plane].mtl_handle_cube_face =
            mtl_new_texture_view_with(mtl_handle, &face_layout);
         view->planes[view_plane].cube_face_gpu_resource_id =
            mtl_texture_get_gpu_resource_id(view->planes[view_plane].mtl_handle_cube_face);
      }

      if (view->vk.usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) {
         struct kk_view_layout rate_layout = view_layout;
         rate_layout.view_type = VK_IMAGE_VIEW_TYPE_2D;
         rate_layout.array_len = 1u;
         rate_layout.num_levels = 1u;
         view->planes[view_plane].mtl_handle_fsr =
            mtl_new_texture_view_with(mtl_handle, &rate_layout);
         view->planes[view_plane].fsr_gpu_resource_id =
            mtl_texture_get_gpu_resource_id(view->planes[view_plane].mtl_handle_fsr);
      }

      if (view->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT) {
         /* For storage images, we can't have any cubes */
         if (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
             view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
            view_layout.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

         VkResult result = kk_sparse_image_view_query(dev, view, mtl_handle, &view_layout, true);
         if (result != VK_SUCCESS) { kk_image_view_finish(dev, view); return result; }
         view->planes[view_plane].mtl_handle_storage =
            mtl_new_texture_view_with(mtl_handle, &view_layout);
         view->planes[view_plane].storage_gpu_resource_id =
            mtl_texture_get_gpu_resource_id(
               view->planes[view_plane].mtl_handle_storage);
      }

      if (view->vk.usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
         bool requires_type_change =
            view_layout.view_type != VK_IMAGE_VIEW_TYPE_3D &&
            view_layout.view_type != VK_IMAGE_VIEW_TYPE_2D_ARRAY;
         bool requires_format_change = view->vk.format != image->vk.format &&
         view_layout.format.mtl != plane->layout.format.mtl;

         /* Required so sampling from input attachments actually return (d, 0,
          * 0, 1) for d/s attachments and render targets cannot have swizzle
          * according to Metal...
          */
         if (view->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
            VkImageViewType original_type = view_layout.view_type;
            bool requires_subresource =
               view_layout.base_array_layer != 0 || view_layout.base_level != 0;
            if (requires_type_change || requires_format_change ||
                requires_subresource) {
               view_layout.view_type = requires_type_change
                                          ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                          : original_type;
               view->planes[view_plane].mtl_handle_input =
                  mtl_new_texture_view_with(mtl_handle, &view_layout);
            } else
               view->planes[view_plane].mtl_handle_input =
                  mtl_retain(mtl_handle);
            view->planes[view_plane].input_gpu_resource_id =
               mtl_texture_get_gpu_resource_id(
                  view->planes[view_plane].mtl_handle_input);
            view_layout.view_type = original_type;
         }

         /* Handle mutable formats */
         if (view->vk.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            view->planes[view_plane].render_is_view = requires_format_change;
            if (requires_format_change) {
               view->planes[view_plane].mtl_handle_render =
                  mtl_new_texture_view_with_no_swizzle(mtl_handle,
                                                       &view_layout);
            } else /* Subresource indices will be set in attachment descriptor */
               view->planes[view_plane].mtl_handle_render =
                  mtl_retain(mtl_handle);
         }
      }
      const char *failure = kk_image_view_plane_error(dev, view, view_plane);
      if (failure) {
         VkResult result = vk_errorf(
            dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "Failed native %s image view (plane=%u format=%u type=%u "
            "mip=%u+%u layer=%u+%u)", failure, view_plane, view->vk.format,
            view->vk.view_type, view->vk.base_mip_level, view->vk.level_count,
            view->vk.base_array_layer, view->vk.layer_count);
         kk_image_view_finish(dev, view);
         return result;
      }
      VkResult alias_result = kk_image_view_init_alias(dev, view, view_plane,
                                                       mtl_handle, &view_layout);
      if (alias_result != VK_SUCCESS) {
         kk_image_view_finish(dev, view);
         return alias_result;
      }
   }

   VkResult result = image->writable_msaa
      ? kk_writable_msaa_view_init(dev, view) : VK_SUCCESS;
   if (result == VK_SUCCESS && layered_bc)
      result = kk_layered_bc_view_init(dev, view);
   if (result != VK_SUCCESS)
      kk_image_view_finish(dev, view);
   return result;
}

void
kk_image_view_finish(struct kk_device *dev, struct kk_image_view *view)
{
   if (view->layered_bc)
      kk_layered_bc_view_finish(dev, view);
   kk_writable_msaa_view_finish(dev, view);
   for (uint8_t plane = 0; plane < view->plane_count; plane++) {
      /* The same owned reference caches either a cube-face view or the
       * mutually exclusive single-layer M11 alias. */
      if (view->planes[plane].mtl_handle_cube_face)
         mtl_release(view->planes[plane].mtl_handle_cube_face);
      if (view->planes[plane].mtl_handle_sampled)
         mtl_release(view->planes[plane].mtl_handle_sampled);

      if (view->planes[plane].mtl_handle_storage)
         mtl_release(view->planes[plane].mtl_handle_storage);

      if (view->planes[plane].mtl_handle_input)
         mtl_release(view->planes[plane].mtl_handle_input);

      if (view->planes[plane].mtl_handle_fsr)
         mtl_release(view->planes[plane].mtl_handle_fsr);

      if (view->planes[plane].transient_private)
         mtl_release(view->planes[plane].transient_private);

      if (view->planes[plane].mtl_handle_render)
         mtl_release(view->planes[plane].mtl_handle_render);

      if (view->planes[plane].mtl_handle_subres)
         mtl_release(view->planes[plane].mtl_handle_subres);
   }

   kk_sparse_image_view_finish(dev, view);
   vk_image_view_finish(&view->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_CreateImageView(VkDevice _device, const VkImageViewCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   struct kk_image_view *view;
   VkResult result;

   view = vk_alloc2(&dev->vk.alloc, pAllocator, sizeof(*view), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   result = kk_image_view_init(dev, view, pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&dev->vk.alloc, pAllocator, view);
      return result;
   }

   *pView = kk_image_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
kk_DestroyImageView(VkDevice _device, VkImageView imageView,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   VK_FROM_HANDLE(kk_image_view, view, imageView);

   if (!view)
      return;

   kk_image_view_finish(dev, view);
   vk_free2(&dev->vk.alloc, pAllocator, view);
}

void
kk_image_view_set_label(struct kk_image_view *iview, const char *label)
{
   for (uint8_t i = 0; i < iview->plane_count; i++) {
      if (iview->planes[i].mtl_handle_sampled)
         mtl_resource_set_label(iview->planes[i].mtl_handle_sampled, label);
      if (iview->planes[i].mtl_handle_storage)
         mtl_resource_set_label(iview->planes[i].mtl_handle_storage, label);
      if (iview->planes[i].mtl_handle_subres)
         mtl_resource_set_label(iview->planes[i].mtl_handle_subres, label);
      /* mtl_handle_render and mtl_handle_input might be the VkImage's handle */
   }
}

#include "kk-writable-msaa-view.h"
