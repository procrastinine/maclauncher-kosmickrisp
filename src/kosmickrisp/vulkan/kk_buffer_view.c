/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_buffer_view.h"
#include "kk_descriptor_types.h"

#include "kk_buffer.h"
#include "kk_device.h"
#include "kk_entrypoints.h"
#include "kk_format.h"
#include "kk_image_layout.h"
#include "kk_nir_lower_vbo.h"
#include "kk_physical_device.h"
#include "kk_rgb32_texel.h"

#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_format.h"

#include "vk_format.h"

static enum pipe_format
kk_texel_buffer_native_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32G32B32_FLOAT: return PIPE_FORMAT_R32_FLOAT;
   case PIPE_FORMAT_R32G32B32_UINT: return PIPE_FORMAT_R32_UINT;
   case PIPE_FORMAT_R32G32B32_SINT: return PIPE_FORMAT_R32_SINT;
   default: return format;
   }
}

VkFormatFeatureFlags2
kk_get_buffer_format_features(struct kk_physical_device *pdev,
                              VkFormat vk_format)
{
   VkFormatFeatureFlags2 features = 0;
   enum pipe_format p_format = vk_format_to_pipe_format(vk_format);

   if (p_format == PIPE_FORMAT_NONE)
      return 0;
   if (!pdev->info.supports_bc_texture_compression && kk_format_is_bc(p_format))
      return 0;

   if (p_format == PIPE_FORMAT_R64_UINT || p_format == PIPE_FORMAT_R64_SINT)
      return !pdev->vk.supported_extensions.EXT_shader_image_atomic_int64 ? 0 :
             VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_ATOMIC_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
             VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

   if (kk_texel_buffer_native_format(p_format) != p_format)
      features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

   const struct kk_va_format *format = kk_get_va_format(p_format);
   if (format) {
      if (format->texel_buffer.read)
         features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;

      if (format->texel_buffer.write) {
         features |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT;
         features |= VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
         features |= VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT;
      }

      /* Only these formats allow atomics for texel buffers */
      if (vk_format == VK_FORMAT_R32_UINT || vk_format == VK_FORMAT_R32_SINT)
         features |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
   }

   if (kk_vbo_supports_format(p_format))
      features |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;

   return features;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_CreateBufferView(VkDevice _device, const VkBufferViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBufferView *pBufferView)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   struct kk_buffer_view *view =
      vk_buffer_view_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*view));
   if (!view)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* VK_WHOLE_SIZE can leave less than one texel after the offset. The
    * valid empty view has a null descriptor; Metal rejects width zero. */
   if (view->vk.elements == 0) {
      *pBufferView = kk_buffer_view_to_handle(view);
      return VK_SUCCESS;
   }

   enum pipe_format p_format = vk_format_to_pipe_format(view->vk.format);
   if (p_format == PIPE_FORMAT_R64_UINT || p_format == PIPE_FORMAT_R64_SINT) {
      struct kk_buffer *buffer = container_of(view->vk.buffer, struct kk_buffer, vk);
      view->texel_buffer_gpu_id = vk_buffer_address(&buffer->vk, view->vk.offset);
      *pBufferView = kk_buffer_view_to_handle(view);
      return VK_SUCCESS;
   }
   struct kk_buffer *buffer = container_of(view->vk.buffer, struct kk_buffer, vk);
   const struct kk_physical_device *pdev = kk_device_physical(dev);
   uint32_t native_prefix_B = 0;
   enum mtl_pixel_format native_rgb = MTL_PIXEL_FORMAT_INVALID;
   if (kk_format_is_rgb32(p_format))
      native_rgb = kk_native_rgb32_texel_format(p_format,
         pdev->info.gpu_apple_family, pdev->info.msl_version,
         buffer->vk.usage, buffer->vk.create_flags, view->vk.offset,
         kk_buffer_mtl_offset(buffer, 0), pdev->info.rgb32_texel_alignment,
         &native_prefix_B);
   enum pipe_format native_format = native_rgb != MTL_PIXEL_FORMAT_INVALID
      ? p_format : kk_texel_buffer_native_format(p_format);
   bool rgb32 = native_format != p_format;
   p_format = native_format;
   const struct kk_va_format *supported_format = kk_get_va_format(p_format);

   /* If we reached here, we support reading at least */
   enum mtl_texture_usage usage = MTL_TEXTURE_USAGE_SHADER_READ;
   if (native_rgb == MTL_PIXEL_FORMAT_INVALID && supported_format->texel_buffer.write)
      usage |= MTL_TEXTURE_USAGE_SHADER_WRITE;

   /* Only these formats allow atomics for texel buffers */
   if (view->vk.format == VK_FORMAT_R32_UINT ||
       view->vk.format == VK_FORMAT_R32_SINT)
      usage |= MTL_TEXTURE_USAGE_SHADER_ATOMIC;

   /* Align relative to the buffer, whose memory binding is already aligned.
    * A common multiple also handles three-component texels without rounding
    * a byte prefix to a fractional texel. */
   uint32_t texel_size = util_format_get_blocksize(p_format);
   uint32_t alignment = KK_MIN_TEXEL_BUFFER_ALIGNMENT;
   while (alignment % texel_size)
      alignment += KK_MIN_TEXEL_BUFFER_ALIGNMENT;
   VkDeviceSize prefix = native_rgb != MTL_PIXEL_FORMAT_INVALID
      ? native_prefix_B : view->vk.offset % alignment;
   view->texel_offset = prefix / texel_size;
   assert(view->texel_offset < KK_MIN_TEXEL_BUFFER_ALIGNMENT);
   assert(!(view->texel_offset & KK_TEXEL_BUFFER_RAW_ADDRESS));

   struct kk_image_layout layout = {
      .width_px = view->vk.elements * (rgb32 ? 3u : 1u) + view->texel_offset,
      .height_px = 1u,
      .depth_px = 1u,
      .layers = 1u,
      .type = MTL_TEXTURE_TYPE_TEXTURE_BUFFER,
      .sample_count_sa = 1u,
      .levels = 1u,
      .linear = true,
      .optimized_layout = false,
      .usage = usage,
      .format = {.pipe = p_format,
                 .mtl = native_rgb != MTL_PIXEL_FORMAT_INVALID
                    ? native_rgb : supported_format->mtl_pixel_format},
      .swizzle =
         {
            .red = supported_format->unswizzle.red,
            .green = supported_format->unswizzle.green,
            .blue = supported_format->unswizzle.blue,
            .alpha = supported_format->unswizzle.alpha,
         },
      .linear_stride_B = view->vk.range + prefix,
   };
   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      view->sparse_view_pool = mtl_new_sparse_buffer_view(buffer->metal.handle, &layout,
         kk_buffer_mtl_offset(buffer, view->vk.offset - prefix), &view->texel_buffer_gpu_id);
   } else
   view->mtl_texel_buffer_handle = mtl_new_texture_with_descriptor_linear(
      buffer->metal.handle, &layout,
      kk_buffer_mtl_offset(buffer, view->vk.offset - prefix));
   if (!view->mtl_texel_buffer_handle && !view->sparse_view_pool) {
      vk_buffer_view_destroy(&dev->vk, pAllocator, &view->vk);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   if (!view->sparse_view_pool)
      view->texel_buffer_gpu_id = mtl_texture_get_gpu_resource_id(view->mtl_texel_buffer_handle);

   if (rgb32)
      view->texel_offset |= KK_TEXEL_BUFFER_RGB32;

   *pBufferView = kk_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
kk_DestroyBufferView(VkDevice _device, VkBufferView bufferView,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   VK_FROM_HANDLE(kk_buffer_view, view, bufferView);

   if (!view)
      return;

   if (view->sparse_view_pool) mtl_release(view->sparse_view_pool);
   mtl_release(view->mtl_texel_buffer_handle);
   vk_buffer_view_destroy(&dev->vk, pAllocator, &view->vk);
}

void
kk_buffer_view_set_label(struct kk_buffer_view *bview, const char *label)
{
   if (bview->mtl_texel_buffer_handle)
      mtl_resource_set_label(bview->mtl_texel_buffer_handle, label);
}
