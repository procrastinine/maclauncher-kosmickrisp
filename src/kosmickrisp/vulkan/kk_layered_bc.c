/* SPDX-License-Identifier: MIT */
#include "kk_layered_bc.h"
#include "kk_bo.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_format.h"
#include "kk_image.h"
#include "kk_image_view.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "vk_format.h"
#include "vk_alloc.h"
#include "util/half_float.h"

struct kk_layered_bc_layer_state {
   struct kk_layered_bc_layer native;
   mtl_texture *storage_plain;
};

struct kk_layered_bc_view {
   struct kk_bo *table;
   uint32_t count;
   struct kk_layered_bc_layer_state layers[];
};

bool
kk_layered_bc_requested(const struct vk_image_view *view)
{
   const bool array_1d = view->image->image_type == VK_IMAGE_TYPE_1D &&
                         view->view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
   const bool array_2d = view->image->image_type == VK_IMAGE_TYPE_2D &&
                        (view->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                         view->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                         view->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
   /* The existing regular-image layout explicitly defines 3D depth slices
    * as 2D array layers for these attachment views. Sparse images cannot
    * carry 2D_ARRAY_COMPATIBLE, and keep their separate kind2 path. */
   const bool slices_3d = view->image->image_type == VK_IMAGE_TYPE_3D &&
                          view->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
                          (view->image->create_flags &
                           VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) &&
                          !(view->image->create_flags &
                            (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                             VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                             VK_IMAGE_CREATE_SPARSE_ALIASED_BIT));
   return (array_1d || array_2d || slices_3d) &&
          view->layer_count > 1 &&
          vk_format_is_compressed(view->image->format) &&
          !vk_format_is_compressed(view->format);
}

VkResult
kk_layered_bc_view_init(struct kk_device *dev, struct kk_image_view *view)
{
   if (!kk_layered_bc_requested(&view->vk))
      return VK_SUCCESS;
   assert(view->plane_count == 1 && view->vk.level_count == 1);
   const uint32_t count = view->vk.layer_count;
   struct kk_layered_bc_view *state = vk_zalloc(&dev->vk.alloc,
      sizeof(*state) + count * sizeof(state->layers[0]), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!state) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   view->layered_bc = state;
   state->count = count;
   const bool needs_sampled = view->vk.usage &
      (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
   const bool needs_storage = view->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT;
   struct kk_sampled_image_descriptor *sampled = NULL;
   struct kk_storage_image_descriptor *storage = NULL;
   /* Render-only aliases need native textures, not shader descriptor tables.
    * For shader views allocate only the tables admitted by the view's usage. */
   if (needs_sampled || needs_storage) {
      const uint64_t sampled_offset = ALIGN_POT(sizeof(struct kk_layered_bc_record), 16);
      const uint64_t storage_offset = sampled_offset + (needs_sampled ?
         (count + 1) * sizeof(struct kk_sampled_image_descriptor) : 0);
      const uint64_t table_bytes = storage_offset + (needs_storage ?
         (count + 1) * sizeof(struct kk_storage_image_descriptor) : 0);
      VkResult result = kk_alloc_bo(dev, &view->vk.base, table_bytes, 16, &state->table);
      if (result != VK_SUCCESS) return result;
      memset(state->table->cpu, 0, table_bytes);
      struct kk_layered_bc_record *record = state->table->cpu;
      record->query.kind = KK_QUERY_RECORD_KIND_LAYERED_BC;
      record->query.view_type = view->vk.view_type;
      record->query.view_size[0] = view->vk.extent.width;
      record->query.view_size[1] = view->vk.extent.height;
      record->query.view_size[2] = count;
      record->query.level_count = record->query.storage_level_count = 1;
      record->layer_count = count;
      if (needs_sampled) {
         record->sampled_descriptors = state->table->gpu + sampled_offset;
         sampled = (void *)((uint8_t *)state->table->cpu + sampled_offset);
      }
      if (needs_storage) {
         record->storage_descriptors = state->table->gpu + storage_offset;
         storage = (void *)((uint8_t *)state->table->cpu + storage_offset);
      }
   }
   const float min_lod = MAX2(view->vk.min_lod - view->vk.base_mip_level, 0.0f);
   const uint16_t min_lod_fp16 = needs_sampled ? _mesa_float_to_half(min_lod) : 0;
   const uint16_t min_lod_uint16 = min_lod;
   struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   struct kk_image_plane *plane = &image->planes[0];
   const struct kk_va_format *format = kk_get_va_format(view->planes[0].format);
   struct kk_view_layout selected = {
      .view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
      .sample_count_sa = 1,
      .format = { .pipe = view->planes[0].format, .mtl = format->mtl_pixel_format },
      .num_levels = 1, .array_len = 1, .min_lod_clamp = min_lod,
   };
   const VkComponentSwizzle swizzle[] = { view->vk.swizzle.r,
      view->vk.swizzle.g, view->vk.swizzle.b, view->vk.swizzle.a };
   /* vk_image_view_init has resolved IDENTITY to concrete channels. */
   uint8_t mapped[4];
   for (unsigned c = 0; c < 4; ++c) {
      VkComponentSwizzle value = swizzle[c];
      mapped[c] = value == VK_COMPONENT_SWIZZLE_ZERO ? PIPE_SWIZZLE_0 :
                  value == VK_COMPONENT_SWIZZLE_ONE ? PIPE_SWIZZLE_1 : value - VK_COMPONENT_SWIZZLE_R;
   }
   util_format_compose_swizzles(format->unswizzle.channels, mapped,
                               selected.swizzle.channels);
   for (uint32_t l = 0; l < count; ++l) {
      struct kk_layered_bc_layer_state *entry = &state->layers[l];
      struct kk_layered_bc_layer *layer = &entry->native;
      if (l == 0) {
         mtl_texture *sampled = view->planes[0].mtl_handle_sampled ?:
                                view->planes[0].mtl_handle_input;
         if (sampled)
            layer->sampled = mtl_retain(sampled);
         if (view->planes[0].mtl_handle_storage)
            layer->storage = mtl_retain(view->planes[0].mtl_handle_storage);
      } else {
         struct kk_image_layout layout = plane->layout;
         layout.type = MTL_TEXTURE_TYPE_2D_ARRAY;
         layout.width_px = view->vk.extent.width;
         layout.height_px = view->vk.extent.height;
         layout.depth_px = layout.layers = layout.levels = 1;
         layout.format.pipe = view->planes[0].format;
         layout.format.mtl = format->mtl_pixel_format;
         VkResult native_result = kk_layered_bc_native_alias_init(dev, plane,
            &layout, kk_get_layer_level_B(&plane->layout,
               view->vk.base_array_layer + l, view->vk.base_mip_level), NULL,
            needs_sampled ? &selected : NULL,
            needs_storage ? &selected : NULL,
            layer);
         if (native_result != VK_SUCCESS)
            return native_result;
      }
      if (layer->sampled) {
         sampled[l].image_gpu_resource_id = mtl_texture_get_gpu_resource_id(layer->sampled);
         sampled[l].aux_resource_id = sampled[l].image_gpu_resource_id;
         sampled[l].image_min_lod_fp16 = min_lod_fp16;
         sampled[l].image_min_lod_uint16 = min_lod_uint16;
      }
      if (layer->storage) {
         const uint64_t array_id = mtl_texture_get_gpu_resource_id(layer->storage);
         storage[l].image_gpu_resource_id = array_id;
         storage[l].image_array_gpu_resource_id = array_id;
         /* Only atomic-capable formats need M11's plain-handle workaround.
          * Other storage operations already use the native array handle. */
         if (format->atomic) {
            struct kk_view_layout plain = selected;
            plain.view_type = VK_IMAGE_VIEW_TYPE_2D;
            entry->storage_plain = mtl_new_texture_view_with(layer->storage, &plain);
            if (!entry->storage_plain)
               return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            storage[l].image_gpu_resource_id =
               mtl_texture_get_gpu_resource_id(entry->storage_plain);
         }
      }
   }
   /* Each table ends with a real all-zero null descriptor. Rejected layers
    * select this entry, preserving the common null-image/robustness path. */
   return VK_SUCCESS;
}

void
kk_layered_bc_view_finish(struct kk_device *dev, struct kk_image_view *view)
{
   struct kk_layered_bc_view *state = view->layered_bc;
   if (!state) return;
   for (uint32_t l = 0; l < state->count; ++l) {
      if (state->layers[l].storage_plain)
         mtl_release(state->layers[l].storage_plain);
      kk_layered_bc_native_alias_finish(&state->layers[l].native);
   }
   if (state->table) kk_destroy_bo(dev, state->table);
   vk_free(&dev->vk.alloc, state);
   view->layered_bc = NULL;
}

void
kk_layered_bc_sampled_descriptor(struct kk_sampled_image_descriptor *desc,
                                 const struct kk_image_view *view)
{
   if (!view->layered_bc || !view->layered_bc->table) return;
   desc->aux_word = view->layered_bc->table->gpu;
   desc->image_min_lod_uint16 |= KK_IMAGE_QUERY_RECORD_TAG;
}

void
kk_layered_bc_storage_descriptor(struct kk_storage_image_descriptor *desc,
                                 const struct kk_image_view *view)
{
   if (!view->layered_bc || !view->layered_bc->table) return;
   desc->image_gpu_resource_id = 0;
   const uint64_t encoded = kk_storage_sparse_query_encode(view->layered_bc->table->gpu);
   memcpy(&desc->texel_offset, &encoded, sizeof(encoded));
}

/* The original image view owns all native aliases through GPU retirement. */
static mtl_texture *
kk_layered_bc_attachment_texture(const struct kk_image_view *view, uint32_t layer)
{
   if (!view->layered_bc)
      return kk_transient_render_texture(view);
   assert(layer < view->layered_bc->count);
   return layer == 0 ? view->planes[0].mtl_handle_render :
                       view->layered_bc->layers[layer].native.base;
}

static bool
kk_layered_bc_attachment_requested(const VkRenderingAttachmentInfo *info)
{
   if (!info)
      return false;
   VK_FROM_HANDLE(kk_image_view, view, info->imageView);
   if (view && view->layered_bc)
      return true;
   if (info->resolveMode != VK_RESOLVE_MODE_NONE) {
      VK_FROM_HANDLE(kk_image_view, resolve, info->resolveImageView);
      return resolve && resolve->layered_bc;
   }
   return false;
}

bool
kk_layered_bc_rendering_info_requested(const VkRenderingInfo *info)
{
   for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
      if (kk_layered_bc_attachment_requested(&info->pColorAttachments[i]))
         return true;
   }
   return kk_layered_bc_attachment_requested(info->pDepthAttachment) ||
          kk_layered_bc_attachment_requested(info->pStencilAttachment);
}

void
kk_layered_bc_attachment_description(
   mtl_render_pass_attachment_descriptor *descriptor,
   const struct kk_attachment *attachment,
   const struct kk_rendering_state *render, bool resolve)
{
   if (!render->layered_bc)
      return;
   const struct kk_image_view *view = resolve ? attachment->resolve_iview :
                                               attachment->iview;
   const uint32_t layer = render->layered_bc_layer;
   assert(view && layer < view->vk.layer_count);
   const bool alias = view->layered_bc != NULL;
   mtl_texture *texture = kk_layered_bc_attachment_texture(view, layer);
   /* Ordinary paired attachments select the same logical slice. Physical raw
    * aliases already begin at the selected original mip/layer. */
   const uint32_t level = alias || view->planes[0].render_is_view ? 0 :
                            view->vk.base_mip_level;
   const uint32_t slice = alias ? 0 :
      layer + (view->planes[0].render_is_view ? 0 : view->vk.base_array_layer);
   if (resolve) {
      mtl_render_pass_attachment_descriptor_set_resolve_texture(descriptor, texture);
      mtl_render_pass_attachment_descriptor_set_resolve_level(descriptor, level);
      mtl_render_pass_attachment_descriptor_set_resolve_slice(descriptor, slice);
   } else {
      mtl_render_pass_attachment_descriptor_set_texture(descriptor, texture);
      mtl_render_pass_attachment_descriptor_set_level(descriptor, level);
      mtl_render_pass_attachment_descriptor_set_slice(descriptor, slice);
   }
}
