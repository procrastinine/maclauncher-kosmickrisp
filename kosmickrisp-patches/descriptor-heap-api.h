/* SPDX-License-Identifier: MIT */
/* Included after the ordinary descriptor writers in kk_descriptor_set.c. */
#include "kk_cmd_buffer.h"
#include "kk_image.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "vk_sampler.h"

#define KK_HEAP_SAMPLER_DESCRIPTOR_SIZE sizeof(uint64_t)
#define KK_HEAP_IMAGE_DESCRIPTOR_SIZE sizeof(struct kk_sampled_image_descriptor)
#define KK_HEAP_METADATA_PAGE_SIZE (64u * 1024u)

struct kk_heap_metadata_page {
   struct kk_heap_metadata_page *next;
   struct kk_bo *bo;
   uint32_t used;
};

struct kk_heap_sampler {
   struct kk_heap_sampler *next;
   struct vk_sampler_state state;
   VkSampler sampler;
   struct kk_sampled_image_descriptor descriptor[3];
   uint64_t gpu_address;
};

struct kk_heap_border {
   struct kk_heap_border *next;
   uint32_t index;
   VkClearColorValue value;
   VkFormat format;
};

struct kk_heap_image_view {
   struct kk_heap_image_view *next;
   uint8_t key[sizeof(struct vk_image_view) -
               offsetof(struct vk_image_view, create_flags)];
   struct kk_image_view view;
};

struct kk_heap_buffer_view {
   struct kk_heap_buffer_view *next;
   VkFormat format;
   uint64_t offset, size;
   VkBufferView view;
};

void
kk_descriptor_heap_init(struct kk_device *dev)
{
   simple_mtx_init(&dev->descriptor_heap_lock, mtx_plain);
}

void
kk_descriptor_heap_finish(struct kk_device *dev)
{
   struct kk_heap_sampler *sampler = dev->heap_samplers;
   while (sampler) {
      struct kk_heap_sampler *next = sampler->next;
      kk_DestroySampler(kk_device_to_handle(dev), sampler->sampler, NULL);
      vk_free(&dev->vk.alloc, sampler);
      sampler = next;
   }
   struct kk_heap_metadata_page *page = dev->heap_metadata_pages;
   while (page) {
      struct kk_heap_metadata_page *next = page->next;
      kk_destroy_bo(dev, page->bo);
      vk_free(&dev->vk.alloc, page);
      page = next;
   }
   struct kk_heap_border *border = dev->heap_borders;
   while (border) {
      struct kk_heap_border *next = border->next;
      vk_free(&dev->vk.alloc, border);
      border = next;
   }
   simple_mtx_destroy(&dev->descriptor_heap_lock);
}

void
kk_descriptor_heap_image_finish(struct kk_device *dev, struct kk_image *image)
{
   struct kk_heap_image_view *view = image->heap_views;
   while (view) {
      struct kk_heap_image_view *next = view->next;
      kk_image_view_finish(dev, &view->view);
      vk_free(&dev->vk.alloc, view);
      view = next;
   }
   image->heap_views = NULL;
}

void
kk_descriptor_heap_buffer_bind(struct kk_device *dev, struct kk_buffer *buffer)
{
   if (!dev->vk.enabled_features.descriptorHeap)
      return;
   simple_mtx_lock(&dev->descriptor_heap_lock);
   if (!buffer->heap_registered) {
      buffer->heap_next = dev->heap_buffers;
      dev->heap_buffers = buffer;
      buffer->heap_registered = true;
   }
   simple_mtx_unlock(&dev->descriptor_heap_lock);
}

void
kk_descriptor_heap_buffer_finish(struct kk_device *dev, struct kk_buffer *buffer)
{
   if (!buffer->heap_registered)
      return;
   simple_mtx_lock(&dev->descriptor_heap_lock);
   struct kk_buffer **link = &dev->heap_buffers;
   while (*link && *link != buffer)
      link = &(*link)->heap_next;
   if (*link)
      *link = buffer->heap_next;
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   struct kk_heap_buffer_view *view = buffer->heap_views;
   while (view) {
      struct kk_heap_buffer_view *next = view->next;
      kk_DestroyBufferView(kk_device_to_handle(dev), view->view, NULL);
      vk_free(&dev->vk.alloc, view);
      view = next;
   }
   buffer->heap_views = NULL;
   buffer->heap_registered = false;
}

/* One cached Vulkan sampler retains every native sampler plane. Descriptor
 * writes copy only its existing shared representation. The cache is keyed by
 * the public runtime's normalized sampler state, including conversion state. */
static VkResult
kk_heap_sampler_locked(struct kk_device *dev,
                        const struct vk_sampler_state *state,
                        struct kk_heap_sampler **out)
{
   for (struct kk_heap_sampler *entry = dev->heap_samplers; entry;
        entry = entry->next) {
      if (!memcmp(&entry->state, state, sizeof(*state))) {
         *out = entry;
         return VK_SUCCESS;
      }
   }
   struct kk_heap_sampler *entry = vk_zalloc(&dev->vk.alloc, sizeof(*entry), 8,
                                              VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!entry)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_ycbcr_conversion conversion = {.state = state->ycbcr_conversion};
   vk_object_base_init(&dev->vk, &conversion.base,
                       VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION);
   VkSamplerYcbcrConversionInfo ycbcr = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
      .conversion = vk_ycbcr_conversion_to_handle(&conversion),
   };
   VkSamplerReductionModeCreateInfo reduction = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
      .pNext = state->has_ycbcr_conversion ? &ycbcr : NULL,
      .reductionMode = state->reduction_mode,
   };
   VkSamplerCustomBorderColorCreateInfoEXT border = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
      .pNext = &reduction,
      .customBorderColor = state->border_color_value,
      .format = state->format,
   };
   VkSamplerBorderColorComponentMappingCreateInfoEXT mapping = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT,
      .pNext = &border,
      .components = state->border_color_component_mapping,
      .srgb = state->image_view_is_srgb,
   };
   VkSamplerCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .pNext = &mapping,
      .flags = state->flags,
      .magFilter = state->mag_filter,
      .minFilter = state->min_filter,
      .mipmapMode = state->mipmap_mode,
      .addressModeU = state->address_mode_u,
      .addressModeV = state->address_mode_v,
      .addressModeW = state->address_mode_w,
      .mipLodBias = state->mip_lod_bias,
      .anisotropyEnable = state->anisotropy_enable,
      .maxAnisotropy = state->max_anisotropy,
      .compareEnable = state->compare_enable,
      .compareOp = state->compare_op,
      .minLod = state->min_lod,
      .maxLod = state->max_lod,
      .borderColor = state->border_color,
      .unnormalizedCoordinates = state->unnormalized_coordinates,
   };
   VkResult result = kk_create_heap_sampler(dev, &info, &entry->sampler);
   if (result == VK_SUCCESS) {
      VkDescriptorImageInfo descriptor = {.sampler = entry->sampler};
      get_sampled_image_view_desc(VK_DESCRIPTOR_TYPE_SAMPLER, &descriptor,
                                  entry->descriptor, sizeof(entry->descriptor),
                                  false);
      /* No conversion object is needed after native planes and descriptor
       * metadata have been prepared. The temporary conversion is not retained. */
      VK_FROM_HANDLE(kk_sampler, sampler, entry->sampler);
      const uint32_t descriptor_planes = sampler->has_border ? 1 : sampler->plane_count;
      for (uint32_t plane = descriptor_planes; plane < 3; ++plane)
         entry->descriptor[plane] = entry->descriptor[descriptor_planes - 1u];
      for (uint32_t plane = 0; plane < 3; ++plane) {
         const uint32_t sampler_plane = MIN2(plane, descriptor_planes - 1u);
         /* This is sampler metadata, never an image descriptor. */
         assert(entry->descriptor[plane].image_gpu_resource_id == 0);
         assert(entry->descriptor[plane].aux_resource_id == 0 &&
                entry->descriptor[plane].aux_word == 0);
         entry->descriptor[plane].aux_resource_id =
            mtl_sampler_get_gpu_resource_id(sampler->planes[sampler_plane].hw->handle);
         if (sampler->has_border)
            entry->descriptor[plane].aux_word =
               mtl_sampler_get_gpu_resource_id(sampler->planes[1].hw->handle);
      }
      sampler->vk.ycbcr_conversion = NULL;
      entry->state = *state;
      entry->next = dev->heap_samplers;
      dev->heap_samplers = entry;
      *out = entry;
   } else {
      vk_free(&dev->vk.alloc, entry);
   }
   vk_object_base_finish(&conversion.base);
   return result;
}

static VkResult
kk_heap_sampler_metadata_locked(struct kk_device *dev,
                                struct kk_heap_sampler *entry)
{
   if (entry->gpu_address)
      return VK_SUCCESS;
   const uint32_t size = sizeof(entry->descriptor);
   static_assert(sizeof(entry->descriptor) <= KK_HEAP_METADATA_PAGE_SIZE,
                 "sampler metadata fits an arena page");
   struct kk_heap_metadata_page *page = dev->heap_metadata_pages;
   uint32_t offset = page ? ALIGN_POT(page->used, 16) : 0;
   if (!page || offset > KK_HEAP_METADATA_PAGE_SIZE - size) {
      page = vk_zalloc(&dev->vk.alloc, sizeof(*page), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!page)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      VkResult result = kk_alloc_bo(dev, &dev->vk.base,
         KK_HEAP_METADATA_PAGE_SIZE, 16, &page->bo);
      if (result != VK_SUCCESS) {
         vk_free(&dev->vk.alloc, page);
         return result;
      }
      page->next = dev->heap_metadata_pages;
      dev->heap_metadata_pages = page;
      offset = 0;
   }
   memcpy((uint8_t *)page->bo->cpu + offset, entry->descriptor, size);
   entry->gpu_address = page->bo->gpu + offset;
   page->used = offset + size;
   return VK_SUCCESS;
}

VkResult
kk_heap_sampler_state(struct kk_device *dev,
                       const struct vk_sampler_state *state,
                       uint64_t *gpu_address)
{
   simple_mtx_lock(&dev->descriptor_heap_lock);
   struct kk_heap_sampler *entry;
   VkResult result = kk_heap_sampler_locked(dev, state, &entry);
   if (result == VK_SUCCESS)
      result = kk_heap_sampler_metadata_locked(dev, entry);
   if (result == VK_SUCCESS)
      *gpu_address = entry->gpu_address;
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_RegisterCustomBorderColorEXT(VkDevice _device,
   const VkSamplerCustomBorderColorCreateInfoEXT *pBorderColor,
   VkBool32 requestIndex, uint32_t *pIndex)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   struct kk_heap_border *entry = vk_zalloc(&dev->vk.alloc, sizeof(*entry), 8,
                                            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!entry)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   uint32_t index = requestIndex ? *pIndex : 0;
   const uint32_t limit = kk_device_physical(dev)->vk.properties.maxCustomBorderColorSamplers;
   for (;;) {
      if (index >= limit) {
         simple_mtx_unlock(&dev->descriptor_heap_lock);
         vk_free(&dev->vk.alloc, entry);
         return vk_error(dev, requestIndex ? VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS
                                           : VK_ERROR_TOO_MANY_OBJECTS);
      }
      bool occupied = false;
      for (struct kk_heap_border *other = dev->heap_borders; other;
           other = other->next)
         occupied |= other->index == index;
      if (!occupied)
         break;
      if (requestIndex) {
         simple_mtx_unlock(&dev->descriptor_heap_lock);
         vk_free(&dev->vk.alloc, entry);
         return vk_error(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
      }
      ++index;
   }
   entry->index = index;
   entry->value = pBorderColor->customBorderColor;
   entry->format = pBorderColor->format;
   entry->next = dev->heap_borders;
   dev->heap_borders = entry;
   *pIndex = index;
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
kk_UnregisterCustomBorderColorEXT(VkDevice _device, uint32_t index)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   struct kk_heap_border **link = &dev->heap_borders;
   while (*link && (*link)->index != index)
      link = &(*link)->next;
   if (*link) {
      struct kk_heap_border *entry = *link;
      *link = entry->next;
      vk_free(&dev->vk.alloc, entry);
   }
   simple_mtx_unlock(&dev->descriptor_heap_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_WriteSamplerDescriptorsEXT(VkDevice _device, uint32_t samplerCount,
   const VkSamplerCreateInfo *pSamplers, const VkHostAddressRangeEXT *pDescriptors)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < samplerCount; ++i) {
      VkSamplerCreateInfo info = pSamplers[i];
      VkSamplerCustomBorderColorCreateInfoEXT border = {
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
         .pNext = info.pNext,
      };
      const VkSamplerCustomBorderColorIndexCreateInfoEXT *indexed =
         vk_find_struct_const(info.pNext, SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT);
      if (indexed) {
         struct kk_heap_border *entry = dev->heap_borders;
         while (entry && entry->index != indexed->index)
            entry = entry->next;
         assert(entry);
         if (!entry) {
            result = vk_error(dev, VK_ERROR_UNKNOWN);
            break;
         }
         border.customBorderColor = entry->value;
         border.format = entry->format;
         info.pNext = &border;
      }
      struct vk_sampler_state state;
      vk_sampler_state_init(&state, &info);
      state.border_color_index = 0;
      struct kk_heap_sampler *sampler;
      result = kk_heap_sampler_locked(dev, &state, &sampler);
      if (result != VK_SUCCESS)
         break;
      result = kk_heap_sampler_metadata_locked(dev, sampler);
      if (result != VK_SUCCESS)
         break;
      assert(pDescriptors[i].size >= KK_HEAP_SAMPLER_DESCRIPTOR_SIZE);
      memcpy(pDescriptors[i].address, &sampler->gpu_address,
              KK_HEAP_SAMPLER_DESCRIPTOR_SIZE);
   }
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return result;
}

/* Normalize through the same runtime view state as ordinary image views. The
 * key includes sliced-3D bounds, minimum LOD, usage and component mapping. */
static VkResult
kk_heap_image_view_locked(struct kk_device *dev,
                           const VkImageViewCreateInfo *info,
                           struct kk_image_view **out)
{
   VK_FROM_HANDLE(kk_image, image, info->image);
   struct vk_image_view key = {};
   vk_image_view_init(&dev->vk, &key, info);
   const size_t offset = offsetof(struct vk_image_view, create_flags);
   const size_t size = sizeof(key) - offset;
   for (struct kk_heap_image_view *entry = image->heap_views; entry;
        entry = entry->next) {
      if (!memcmp((const char *)&key + offset,
                   entry->key, size)) {
         vk_image_view_finish(&key);
         *out = &entry->view;
         return VK_SUCCESS;
      }
   }
   struct kk_heap_image_view *entry = vk_zalloc(&dev->vk.alloc, sizeof(*entry), 8,
                                                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!entry) {
      vk_image_view_finish(&key);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memcpy(entry->key, (const char *)&key + offset, size);
   vk_image_view_finish(&key);
   VkResult result = kk_image_view_init(dev, &entry->view, info);
   if (result != VK_SUCCESS) {
      vk_free(&dev->vk.alloc, entry);
      return result;
   }
   entry->next = image->heap_views;
   image->heap_views = entry;
   *out = &entry->view;
   return VK_SUCCESS;
}

static struct kk_buffer *
kk_heap_buffer_from_address(struct kk_device *dev, uint64_t address, uint64_t size)
{
   for (struct kk_buffer *buffer = dev->heap_buffers; buffer;
        buffer = buffer->heap_next) {
      const uint64_t base = buffer->vk.device_address;
      if (base && address >= base && address - base <= buffer->vk.size &&
          size <= buffer->vk.size - (address - base))
         return buffer;
   }
   return NULL;
}

static VkResult
kk_heap_texel_view_locked(struct kk_device *dev,
                          const VkTexelBufferDescriptorInfoEXT *info,
                          VkDescriptorType type, void *destination,
                          uint32_t descriptor_size)
{
   const uint64_t address = info->addressRange.address;
   const uint64_t size = info->addressRange.size;
   struct kk_buffer *buffer = kk_heap_buffer_from_address(dev, address, size);
   assert(buffer && "texel descriptor address is backed by a live buffer");
   if (!buffer)
      return vk_error(dev, VK_ERROR_UNKNOWN);
   const uint64_t offset = address - buffer->vk.device_address;
   struct kk_heap_buffer_view *entry = buffer->heap_views;
   while (entry && (entry->format != info->format ||
                    entry->offset != offset || entry->size != size))
      entry = entry->next;
   if (!entry) {
      entry = vk_zalloc(&dev->vk.alloc, sizeof(*entry), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!entry)
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      const VkBufferViewCreateInfo create = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
         .buffer = kk_buffer_to_handle(buffer),
         .format = info->format,
         .offset = offset,
         .range = size,
      };
      VkResult result = kk_CreateBufferView(kk_device_to_handle(dev), &create,
                                             NULL, &entry->view);
      if (result != VK_SUCCESS) {
         vk_free(&dev->vk.alloc, entry);
         return result;
      }
      entry->format = info->format;
      entry->offset = offset;
      entry->size = size;
      entry->next = buffer->heap_views;
      buffer->heap_views = entry;
   }
   /* Use the ordinary writer and its declared stride, including any metadata
    * appended by another capability. The local target describes one entry. */
   _Alignas(struct kk_descriptor_set_layout) uint8_t layout_storage[
      sizeof(struct kk_descriptor_set_layout) +
      sizeof(struct kk_descriptor_set_binding_layout)] = {};
   struct kk_descriptor_set_layout *layout = (void *)layout_storage;
   layout->binding_count = 1;
   layout->binding[0].type = type;
   layout->binding[0].stride = descriptor_size;
   layout->binding[0].array_size = 1;
   struct kk_descriptor_set target = {
      .layout = layout,
      .size = descriptor_size,
      .mapped_ptr = destination,
   };
   write_buffer_view_desc(&target, entry->view, 0, 0);
   return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceSize VKAPI_CALL
kk_GetPhysicalDeviceDescriptorSizeEXT(VkPhysicalDevice physicalDevice,
                                      VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      return KK_HEAP_SAMPLER_DESCRIPTOR_SIZE;
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
      uint32_t stride, alignment;
      kk_descriptor_stride_align_for_type(type, NULL, &stride, &alignment);
      return stride;
   }
   default:
      return 0;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_GetImageOpaqueCaptureDataEXT(VkDevice _device, uint32_t imageCount,
                                 const VkImage *pImages,
                                 VkHostAddressRangeEXT *pDatas)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   /* descriptorHeapCaptureReplay is not advertised. */
   return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_WriteResourceDescriptorsEXT(VkDevice _device, uint32_t resourceCount,
   const VkResourceDescriptorInfoEXT *pResources,
   const VkHostAddressRangeEXT *pDescriptors)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < resourceCount && result == VK_SUCCESS; ++i) {
      const VkResourceDescriptorInfoEXT *resource = &pResources[i];
      const VkHostAddressRangeEXT *dst = &pDescriptors[i];
      const size_t size = kk_GetPhysicalDeviceDescriptorSizeEXT(
         vk_physical_device_to_handle(dev->vk.physical), resource->type);
      assert(dst->size >= size);
      memset(dst->address, 0, size);
      switch (resource->type) {
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
         const VkImageDescriptorInfoEXT *info = resource->data.pImage;
         if (!info)
            break;
         struct kk_image_view *view;
         result = kk_heap_image_view_locked(dev, info->pView, &view);
         if (result != VK_SUCCESS)
            break;
         const VkDescriptorImageInfo descriptor = {
            .imageView = kk_image_view_to_handle(view),
            .imageLayout = info->layout,
         };
         if (resource->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            get_storage_image_view_desc(NULL, &descriptor, dst->address, dst->size);
         else {
            struct kk_sampled_image_descriptor planes[3] = {};
            get_sampled_image_view_desc(resource->type, &descriptor,
               planes, sizeof(planes),
               resource->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
            /* Single-plane auxiliary words belong to the shared writer.
             * Multi-plane images use them for their remaining native IDs. */
            memcpy(dst->address, &planes[0], KK_HEAP_IMAGE_DESCRIPTOR_SIZE);
         }
         break;
      }
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
         const VkDeviceAddressRangeEXT *range = resource->data.pAddressRange;
         if (range) {
            struct kk_buffer *buffer = range->address ?
               kk_heap_buffer_from_address(dev, range->address, range->size) : NULL;
            if (range->address && !buffer) {
               result = vk_error(dev, VK_ERROR_UNKNOWN);
               break;
            }
            const struct kk_addr_range addr_range = {
               .addr = range->address,
               .range = range->address ? range->size : 0,
            };
            const struct kk_buffer_address descriptor =
               kk_buffer_descriptor(buffer, addr_range);
            memcpy(dst->address, &descriptor, sizeof(descriptor));
         }
         break;
      }
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
         const VkTexelBufferDescriptorInfoEXT *info = resource->data.pTexelBuffer;
         if (info && info->addressRange.address && info->addressRange.size) {
            result = kk_heap_texel_view_locked(dev, info, resource->type,
                                                dst->address, size);
         }
         break;
      }
      default:
         UNREACHABLE("unsupported resource heap descriptor type");
      }
   }
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindSamplerHeapEXT(VkCommandBuffer commandBuffer,
                          const VkBindHeapInfoEXT *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   cmd->state.gfx.descriptors.heap_sampler = info->heapRange.address;
   cmd->state.cs.descriptors.heap_sampler = info->heapRange.address;
   cmd->state.gfx.descriptors.root_dirty = true;
   cmd->state.cs.descriptors.root_dirty = true;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindResourceHeapEXT(VkCommandBuffer commandBuffer,
                           const VkBindHeapInfoEXT *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   cmd->state.gfx.descriptors.heap_resource = info->heapRange.address;
   cmd->state.cs.descriptors.heap_resource = info->heapRange.address;
   cmd->state.gfx.descriptors.root_dirty = true;
   cmd->state.cs.descriptors.root_dirty = true;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdPushDataEXT(VkCommandBuffer commandBuffer, const VkPushDataInfoEXT *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   kk_descriptor_push_update(&cmd->state.gfx.descriptors, info->offset,
                              info->data.address, info->data.size);
   kk_descriptor_push_update(&cmd->state.cs.descriptors, info->offset,
                              info->data.address, info->data.size);
}
