/* SPDX-License-Identifier: MIT */
/* Included after the ordinary descriptor writers in kk_descriptor_set.c. */
#include "kk_cmd_buffer.h"
#include "kk_image.h"
#include "kk_format.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "vk_sampler.h"
#include "util/hash_table.h"

#define KK_HEAP_SAMPLER_DESCRIPTOR_SIZE sizeof(uint64_t)
#define KK_HEAP_IMAGE_DESCRIPTOR_SIZE sizeof(struct kk_sampled_image_descriptor)
#define KK_HEAP_METADATA_PAGE_SIZE (64u * 1024u)

struct kk_heap_metadata_page {
   struct kk_heap_metadata_page *next;
   struct kk_bo *bo;
   uint32_t used;
};

/* The common case repeats one view/state. Keep that hit as a direct equality
 * check; the Mesa hash index bounds lookup work when a cache has many entries. */
struct kk_heap_cache {
   struct hash_table table;
   const void *last_key;
   void *last_data;
};

struct kk_heap_sampler {
   struct vk_sampler_state state;
   VkSampler sampler;
   struct kk_sampled_image_descriptor descriptor[3];
   uint64_t gpu_address;
};

struct kk_heap_border {
   VkClearColorValue value;
   VkFormat format;
   bool used;
};

/* Existing public border indices are dense and bounded by the device limit.
 * Grow only as needed; lookup/reuse needs no per-entry allocation or scan. */
struct kk_heap_border_registry {
   uint32_t capacity, first_free;
   struct kk_heap_border entries[];
};

struct kk_heap_image_view {
   uint8_t key[sizeof(struct vk_image_view) -
               offsetof(struct vk_image_view, create_flags)];
   struct kk_image_view view;
};

static uint32_t
kk_heap_sampler_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(struct vk_sampler_state));
}

static bool
kk_heap_sampler_equal(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(struct vk_sampler_state)) == 0;
}

static uint32_t
kk_heap_image_view_hash(const void *key)
{
   return _mesa_hash_data(key, sizeof(((struct kk_heap_image_view *)0)->key));
}

static bool
kk_heap_image_view_equal(const void *a, const void *b)
{
   return memcmp(a, b, sizeof(((struct kk_heap_image_view *)0)->key)) == 0;
}

/* Only an actual heap image/embedded-sampler request creates a cache.
 * Unused device and image caches retain their existing NULL slots. */
static VkResult
kk_heap_cache_init(struct kk_device *dev, struct kk_heap_cache **cache,
                   uint32_t (*hash)(const void *),
                   bool (*equal)(const void *, const void *))
{
   if (*cache)
      return VK_SUCCESS;
   *cache = vk_zalloc(&dev->vk.alloc, sizeof(**cache), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!*cache)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   _mesa_hash_table_init(&(*cache)->table, NULL, hash, equal);
   return VK_SUCCESS;
}

static void *
kk_heap_cache_lookup(struct kk_heap_cache *cache, const void *key,
                     bool (*equal)(const void *, const void *))
{
   if (cache->last_key && equal(cache->last_key, key))
      return cache->last_data;
   struct hash_entry *entry = _mesa_hash_table_search(&cache->table, key);
   if (!entry)
      return NULL;
   cache->last_key = entry->key;
   cache->last_data = entry->data;
   return entry->data;
}

static bool
kk_heap_cache_insert(struct kk_heap_cache *cache, const void *key, void *data)
{
   if (!_mesa_hash_table_insert(&cache->table, key, data))
      return false;
   cache->last_key = key;
   cache->last_data = data;
   return true;
}

void
kk_descriptor_heap_init(struct kk_device *dev)
{
   simple_mtx_init(&dev->descriptor_heap_lock, mtx_plain);
}

void
kk_descriptor_heap_finish(struct kk_device *dev)
{
   if (dev->heap_samplers) {
      hash_table_foreach(&dev->heap_samplers->table, entry) {
         struct kk_heap_sampler *sampler = entry->data;
         kk_DestroySampler(kk_device_to_handle(dev), sampler->sampler, NULL);
         vk_free(&dev->vk.alloc, sampler);
      }
      _mesa_hash_table_fini(&dev->heap_samplers->table, NULL);
      vk_free(&dev->vk.alloc, dev->heap_samplers);
   }
   struct kk_heap_metadata_page *page = dev->heap_metadata_pages;
   while (page) {
      struct kk_heap_metadata_page *next = page->next;
      kk_destroy_bo(dev, page->bo);
      vk_free(&dev->vk.alloc, page);
      page = next;
   }
   vk_free(&dev->vk.alloc, dev->heap_borders);
   simple_mtx_destroy(&dev->descriptor_heap_lock);
}

void
kk_descriptor_heap_image_finish(struct kk_device *dev, struct kk_image *image)
{
   if (image->heap_views) {
      hash_table_foreach(&image->heap_views->table, entry) {
         struct kk_heap_image_view *view = entry->data;
         kk_image_view_finish(dev, &view->view);
         vk_free(&dev->vk.alloc, view);
      }
      _mesa_hash_table_fini(&image->heap_views->table, NULL);
      vk_free(&dev->vk.alloc, image->heap_views);
   }
   image->heap_views = NULL;
}

void
kk_descriptor_buffer_register_locked(struct kk_device *dev,
                                      struct kk_buffer *buffer)
{
   if (!buffer->heap_registered) {
      buffer->heap_next = dev->heap_buffers;
      dev->heap_buffers = buffer;
      buffer->heap_registered = true;
   }
}

void
kk_descriptor_heap_buffer_bind(struct kk_device *dev, struct kk_buffer *buffer)
{
   if (!dev->vk.enabled_features.descriptorHeap)
      return;
   simple_mtx_lock(&dev->descriptor_heap_lock);
   kk_descriptor_buffer_register_locked(dev, buffer);
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
   VkResult result = kk_heap_cache_init(dev, &dev->heap_samplers,
                                        kk_heap_sampler_hash, kk_heap_sampler_equal);
   if (result != VK_SUCCESS)
      return result;
   struct kk_heap_sampler *cached = kk_heap_cache_lookup(dev->heap_samplers,
                                                          state, kk_heap_sampler_equal);
   if (cached) {
      *out = cached;
      return VK_SUCCESS;
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
   result = kk_create_heap_sampler(dev, &info, &entry->sampler,
                                             KK_SAMPLER_PARAMS_HEAP_LOCK_HELD);
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
      if (!kk_heap_cache_insert(dev->heap_samplers, &entry->state, entry)) {
         kk_DestroySampler(kk_device_to_handle(dev), entry->sampler, NULL);
         vk_free(&dev->vk.alloc, entry);
         result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      } else {
         *out = entry;
      }
   } else {
      vk_free(&dev->vk.alloc, entry);
   }
   vk_object_base_finish(&conversion.base);
   return result;
}

#include "kk_sampler_params_impl.h"

static VkResult
kk_heap_sampler_metadata_locked(struct kk_device *dev,
                                struct kk_heap_sampler *entry)
{
   if (entry->gpu_address)
      return VK_SUCCESS;
   const uint32_t size = sizeof(entry->descriptor);
   static_assert(sizeof(entry->descriptor) <= KK_HEAP_METADATA_PAGE_SIZE,
                 "sampler metadata fits an arena page");
   return kk_heap_metadata_alloc_locked(dev, entry->descriptor, size, 16,
                                        &entry->gpu_address);
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

VkResult
kk_heap_embedded_sampler_table(struct kk_device *dev, uint32_t count,
   const struct vk_sampler_state *states, uint64_t *gpu_address)
{
   *gpu_address = 0;
   if (!count) return VK_SUCCESS;
   if (!states || count > dev->vk.physical->properties.maxDescriptorHeapEmbeddedSamplers ||
       count > KK_HEAP_METADATA_PAGE_SIZE / sizeof(uint64_t))
      return vk_error(dev, VK_ERROR_UNKNOWN);
   uint64_t single_address = 0, table = 0;
   uint64_t *addresses = &single_address;
   simple_mtx_lock(&dev->descriptor_heap_lock);
   VkResult result = VK_SUCCESS;
   if (count > 1) {
      void *cpu;
      result = kk_heap_metadata_reserve_locked(dev, count * sizeof(*addresses),
                                               8, &cpu, &table);
      if (result != VK_SUCCESS) {
         simple_mtx_unlock(&dev->descriptor_heap_lock);
         return result;
      }
      addresses = cpu;
   }
   for (uint32_t i = 0; i < count; ++i) {
      struct kk_heap_sampler *sampler;
      result = kk_heap_sampler_locked(dev, &states[i], &sampler);
      if (result != VK_SUCCESS) break;
      result = kk_heap_sampler_metadata_locked(dev, sampler);
      if (result != VK_SUCCESS) break;
      addresses[i] = sampler->gpu_address;
   }
   /* Publish only a complete immutable table. Each slot was initialized in
    * its final resident allocation; no host temporary/copy/free is needed. */
   if (result == VK_SUCCESS)
      *gpu_address = count == 1 ? single_address : table;
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_RegisterCustomBorderColorEXT(VkDevice _device,
   const VkSamplerCustomBorderColorCreateInfoEXT *pBorderColor,
   VkBool32 requestIndex, uint32_t *pIndex)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   const uint32_t limit = kk_device_physical(dev)->vk.properties.maxCustomBorderColorSamplers;
   uint32_t index = requestIndex ? *pIndex :
      (dev->heap_borders ? dev->heap_borders->first_free : 0);
   VkResult result = VK_SUCCESS;
   if (index >= limit) {
      result = vk_error(dev, requestIndex ? VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS
                                         : VK_ERROR_TOO_MANY_OBJECTS);
      goto out;
   }
   struct kk_heap_border_registry *registry = dev->heap_borders;
   const uint32_t old_capacity = registry ? registry->capacity : 0;
   if (index >= old_capacity) {
      const uint32_t capacity = MIN2(limit, MAX2(index + 1u,
                                                old_capacity ? old_capacity * 2u : 16u));
      const size_t size = sizeof(*registry) + capacity * sizeof(registry->entries[0]);
      registry = vk_realloc(&dev->vk.alloc, registry, size, 8,
                              VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!registry) {
         result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto out;
      }
      if (!old_capacity)
         registry->first_free = 0;
      memset(&registry->entries[old_capacity], 0,
             (capacity - old_capacity) * sizeof(registry->entries[0]));
      registry->capacity = capacity;
      dev->heap_borders = registry;
   }
   if (registry->entries[index].used) {
      result = vk_error(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
      goto out;
   }
   registry->entries[index] = (struct kk_heap_border){
      .value = pBorderColor->customBorderColor, .format = pBorderColor->format, .used = true,
   };
   if (index == registry->first_free) {
      do {
         registry->first_free++;
      } while (registry->first_free < registry->capacity &&
               registry->entries[registry->first_free].used);
   }
   *pIndex = index;
out:
   simple_mtx_unlock(&dev->descriptor_heap_lock);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
kk_UnregisterCustomBorderColorEXT(VkDevice _device, uint32_t index)
{
   VK_FROM_HANDLE(kk_device, dev, _device);
   simple_mtx_lock(&dev->descriptor_heap_lock);
   struct kk_heap_border_registry *registry = dev->heap_borders;
   if (registry && index < registry->capacity && registry->entries[index].used) {
      registry->entries[index].used = false;
      registry->first_free = MIN2(registry->first_free, index);
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
         struct kk_heap_border_registry *registry = dev->heap_borders;
         const bool valid = registry && indexed->index < registry->capacity &&
                            registry->entries[indexed->index].used;
         assert(valid);
         if (!valid) {
            result = vk_error(dev, VK_ERROR_UNKNOWN);
            break;
         }
         border.customBorderColor = registry->entries[indexed->index].value;
         border.format = registry->entries[indexed->index].format;
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
   VkResult result = kk_heap_cache_init(dev, &image->heap_views,
                                        kk_heap_image_view_hash, kk_heap_image_view_equal);
   if (result != VK_SUCCESS) {
      vk_image_view_finish(&key);
      return result;
   }
   struct kk_heap_image_view *cached = kk_heap_cache_lookup(image->heap_views,
      (const char *)&key + offset, kk_heap_image_view_equal);
   if (cached) {
      vk_image_view_finish(&key);
      *out = &cached->view;
      return VK_SUCCESS;
   }
   struct kk_heap_image_view *entry = vk_zalloc(&dev->vk.alloc, sizeof(*entry), 8,
                                                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!entry) {
      vk_image_view_finish(&key);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memcpy(entry->key, (const char *)&key + offset, size);
   vk_image_view_finish(&key);
   result = kk_image_view_init(dev, &entry->view, info);
   if (result != VK_SUCCESS) {
      vk_free(&dev->vk.alloc, entry);
      return result;
   }
   if (!kk_heap_cache_insert(image->heap_views, entry->key, entry)) {
      kk_image_view_finish(dev, &entry->view);
      vk_free(&dev->vk.alloc, entry);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   *out = &entry->view;
   return VK_SUCCESS;
}

struct kk_buffer *
kk_descriptor_buffer_from_address_locked(struct kk_device *dev,
                                          uint64_t address, uint64_t size)
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

VkResult
kk_descriptor_write_raw_texel_buffer_locked(struct kk_device *dev,
   const struct kk_buffer *selected, uint64_t address, uint64_t size,
   VkFormat format, VkDescriptorType type, void *destination,
   uint32_t descriptor_size)
{
   if ((type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
        type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) ||
       !destination || descriptor_size < sizeof(struct kk_texel_buffer_descriptor))
      return VK_ERROR_UNKNOWN;
   struct kk_texel_buffer_descriptor descriptor = {};
   /* Nulls have no selected virtual source and do not read ignored format or
    * range state. They keep the common all-zero representation. */
   if (address) {
      if (!selected || !selected->vk.device_address ||
          address < selected->vk.device_address)
         return VK_ERROR_UNKNOWN;
      const uint64_t offset = address - selected->vk.device_address;
      if (offset > selected->vk.size || size > selected->vk.size - offset)
         return VK_ERROR_UNKNOWN;
      const VkFormatFeatureFlags2 needed = type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
         ? VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT
         : VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT;
      if (!(kk_get_buffer_format_features(kk_device_physical(dev), format) & needed))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      const enum pipe_format pipe = vk_format_to_pipe_format(format);
      if (!kk_raw_texel_format_supported(pipe))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      const uint32_t texel_size = util_format_get_blocksize(pipe);
      const uint64_t count = size / texel_size;
      if (count > UINT32_MAX)
         return VK_ERROR_UNKNOWN;
      descriptor.image.image_gpu_resource_id = address;
      descriptor.image.texel_offset = KK_TEXEL_BUFFER_RAW_ADDRESS | pipe;
      descriptor.image.texel_count = count;
      descriptor.sparse = kk_sparse_buffer_range_descriptor(selected, offset,
                                                            texel_size, false);
   }
   /* Validation is complete. No remaining operation can allocate or fail. */
   memset(destination, 0, descriptor_size);
   memcpy(destination, &descriptor, sizeof(descriptor));
   return VK_SUCCESS;
}

bool
kk_descriptor_raw_texel_sources_equal_locked(const struct kk_buffer *a,
   const struct kk_buffer *b, uint64_t address, uint64_t size, VkFormat format)
{
   if (!a || !b || !a->vk.device_address || !b->vk.device_address ||
       address < a->vk.device_address || address < b->vk.device_address)
      return false;
   const uint64_t a_offset = address - a->vk.device_address;
   const uint64_t b_offset = address - b->vk.device_address;
   if (a_offset > a->vk.size || size > a->vk.size - a_offset ||
       b_offset > b->vk.size || size > b->vk.size - b_offset ||
       a->sparse_record != b->sparse_record)
      return false;
   (void)format; /* Format validation/encoding belongs to the selected writer. */
   if (a == b)
      return true;
   if (a->sparse_residency || b->sparse_residency ||
       a->sparse_record || b->sparse_record)
      return false;
   /* The bridge checks retained native-buffer intervals or actual placement
    * heap identity/intervals. Numerical VA coverage alone is not proof. */
   if (!a->metal.handle || !b->metal.handle ||
       a->metal.offset > UINT64_MAX - a_offset ||
       b->metal.offset > UINT64_MAX - b_offset)
      return false;
   const uint64_t native_a = a->metal.offset + a_offset;
   const uint64_t native_b = b->metal.offset + b_offset;
   return mtl_buffer_ranges_alias(a->metal.handle, native_a,
                                  b->metal.handle, native_b, size);
}

VkResult
kk_descriptor_write_texel_buffer_locked(struct kk_device *dev,
   uint64_t address, uint64_t size, VkFormat format, VkDescriptorType type,
   void *destination, uint32_t descriptor_size)
{
   const struct kk_buffer *selected = NULL;
   if (address) {
      for (struct kk_buffer *buffer = dev->heap_buffers; buffer;
           buffer = buffer->heap_next) {
         const uint64_t base = buffer->vk.device_address;
         if (!base || address < base || address - base > buffer->vk.size ||
             size > buffer->vk.size - (address - base))
            continue;
         if (selected && !kk_descriptor_raw_texel_sources_equal_locked(selected,
               buffer, address, size, format))
            return VK_ERROR_UNKNOWN;
         if (!selected)
            selected = buffer;
      }
      if (!selected)
         return VK_ERROR_UNKNOWN;
   }
   /* All address-only typed producers use the same allocation-free mode.
    * Explicit VkBufferView writers remain native and never enter this route. */
   return kk_descriptor_write_raw_texel_buffer_locked(dev, selected, address,
      size, format, type, destination, descriptor_size);
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
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
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
               kk_descriptor_buffer_from_address_locked(dev, range->address,
                                                        range->size) : NULL;
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
            result = kk_descriptor_write_texel_buffer_locked(dev,
               info->addressRange.address, info->addressRange.size, info->format,
               resource->type, dst->address, size);
         }
         break;
      }
      case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
         const VkDeviceAddressRangeEXT *range = resource->data.pAddressRange;
         const struct kk_as_descriptor desc = kk_as_descriptor_from_range(
            range ? range->address : 0, range ? range->size : 0);
         memcpy(dst->address, &desc, sizeof(desc));
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
   struct kk_descriptor_state *graphics = &cmd->state.gfx.descriptors;
   struct kk_descriptor_state *compute = &cmd->state.cs.descriptors;
   const uint64_t address = info->heapRange.address;
   graphics->root_dirty |= graphics->descriptor_heap && graphics->heap_sampler != address;
   compute->root_dirty |= compute->descriptor_heap && compute->heap_sampler != address;
   graphics->heap_sampler = address;
   compute->heap_sampler = address;
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBindResourceHeapEXT(VkCommandBuffer commandBuffer,
                           const VkBindHeapInfoEXT *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_descriptor_state *graphics = &cmd->state.gfx.descriptors;
   struct kk_descriptor_state *compute = &cmd->state.cs.descriptors;
   const uint64_t address = info->heapRange.address;
   graphics->root_dirty |= graphics->descriptor_heap && graphics->heap_resource != address;
   compute->root_dirty |= compute->descriptor_heap && compute->heap_resource != address;
   graphics->heap_resource = address;
   compute->heap_resource = address;
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

/* Global heap/push-data state also applies to RT. Only enabled devices install
 * these handlers; no shader, allocation or binding is added to ordinary ones. */
static VKAPI_ATTR void VKAPI_CALL
kk_rt_CmdBindSamplerHeapEXT(VkCommandBuffer cb, const VkBindHeapInfoEXT *info)
{
   kk_CmdBindSamplerHeapEXT(cb, info);
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, cb);
   if (cmd->state.rt) {
      struct kk_descriptor_state *desc = &cmd->state.rt->descriptors;
      desc->root_dirty |= desc->descriptor_heap && desc->heap_sampler != info->heapRange.address;
      desc->heap_sampler = info->heapRange.address;
   }
}

static VKAPI_ATTR void VKAPI_CALL
kk_rt_CmdBindResourceHeapEXT(VkCommandBuffer cb, const VkBindHeapInfoEXT *info)
{
   kk_CmdBindResourceHeapEXT(cb, info);
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, cb);
   if (cmd->state.rt) {
      struct kk_descriptor_state *desc = &cmd->state.rt->descriptors;
      desc->root_dirty |= desc->descriptor_heap && desc->heap_resource != info->heapRange.address;
      desc->heap_resource = info->heapRange.address;
   }
}

static VKAPI_ATTR void VKAPI_CALL
kk_rt_CmdPushDataEXT(VkCommandBuffer cb, const VkPushDataInfoEXT *info)
{
   kk_CmdPushDataEXT(cb, info);
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, cb);
   struct kk_descriptor_state *desc = kk_rt_descriptors(cmd);
   if (desc)
      kk_descriptor_push_update(desc, info->offset, info->data.address, info->data.size);
}

void
kk_rt_heap_dispatch_init(struct kk_device *dev)
{
   dev->vk.dispatch_table.CmdBindSamplerHeapEXT = kk_rt_CmdBindSamplerHeapEXT;
   dev->vk.dispatch_table.CmdBindResourceHeapEXT = kk_rt_CmdBindResourceHeapEXT;
   dev->vk.dispatch_table.CmdPushDataEXT = kk_rt_CmdPushDataEXT;
}
