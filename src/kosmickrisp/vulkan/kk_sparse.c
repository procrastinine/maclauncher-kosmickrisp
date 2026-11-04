/* SPDX-License-Identifier: MIT AND Apache-2.0
 * Copyright (c) 2015-2026 The Brenwill Workshop Ltd.
 * Placement sparse resources, per-layer mip tails and opaque row mapping use
 * the technique in MetalSharp's MoltenVK adaptation (sparse-origins.json).
 */
#include "kk_sparse.h"
#include "kk_buffer.h"
#include "kk_buffer_view.h"
#include "kk_device.h"
#include "kk_device_memory.h"
#include "kk_descriptor_types.h"
#include "kk_format.h"
#include "kk_image.h"
#include "kk_image_view.h"
#include "kk_physical_device.h"
#include "kk_queue.h"
#include "kk_entrypoints.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "vk_format.h"
#include "util/u_memory.h"
#include "kk_standard_sparse_3d.h"

uint32_t
kk_sparse_memory_type_bits(struct kk_physical_device *pdev)
{
   uint32_t bits = 0;
   for (unsigned i = 0; i < pdev->mem_type_count; ++i)
      if (!(pdev->mem_types[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)))
         bits |= BITFIELD_BIT(i);
   return bits;
}

static void
kk_sparse_residency(struct kk_device *dev, void *resource, bool add)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   if (add)
      mtl_residency_set_add_allocation(dev->residency_set.handle, resource);
   else
      mtl_residency_set_remove_allocation(dev->residency_set.handle, resource);
   dev->residency_set.dirty = true;
   simple_mtx_unlock(&dev->residency_set.mutex);
}

/* A stable radix directory lets update-after-bind descriptors retain a
 * 32-bit record index. Nodes are only published once; existing records never
 * move when another sparse buffer is created. The 12/10/10 split covers the
 * complete index, with 32 KiB leaves allocated only as needed. */
struct kk_sparse_record_node {
   mtl_buffer *buffer;
   struct kk_sparse_record_node *children[1024];
};
struct kk_sparse_range {
   uint64_t start, end, record_address, reserved;
};
struct kk_sparse_range_version {
   mtl_buffer *buffer;
   struct kk_sparse_range_version *next;
};
struct kk_sparse_device_state {
   simple_mtx_t mutex;
   mtl_buffer *root;
   struct kk_sparse_record_node *children[4096];
   uint32_t next_record;
   struct util_dynarray ranges, retired_maps;
   struct kk_sparse_range_version *current, *retired;
   unsigned submissions;
};

static struct kk_sparse_device_state *
kk_sparse_state(struct kk_device *dev)
{
   return p_atomic_read(&dev->sparse);
}

static void
kk_sparse_reclaim(struct kk_device *dev)
{
   struct kk_sparse_device_state *state = kk_sparse_state(dev);
   if (state->submissions) return;
   while (state->retired) {
      struct kk_sparse_range_version *version = state->retired;
      state->retired = version->next;
      kk_sparse_residency(dev, version->buffer, false);
      mtl_release(version->buffer); free(version);
   }
   util_dynarray_foreach(&state->retired_maps, mtl_buffer *, map) {
      kk_sparse_residency(dev, *map, false);
      mtl_release(*map);
   }
   util_dynarray_clear(&state->retired_maps);
}

bool
kk_sparse_submission_begin(struct kk_device *dev)
{
   if (!kk_sparse_state(dev) || !kk_sparse_residency_used(dev)) return false;
   simple_mtx_lock(&kk_sparse_state(dev)->mutex);
   kk_sparse_state(dev)->submissions++;
   simple_mtx_unlock(&kk_sparse_state(dev)->mutex);
   return true;
}

void
kk_sparse_submission_end(struct kk_device *dev)
{
   if (!kk_sparse_state(dev)) return;
   simple_mtx_lock(&kk_sparse_state(dev)->mutex);
   assert(kk_sparse_state(dev)->submissions);
   kk_sparse_state(dev)->submissions--;
   kk_sparse_reclaim(dev);
   simple_mtx_unlock(&kk_sparse_state(dev)->mutex);
}

static int
kk_sparse_range_compare(const void *a, const void *b)
{
   const struct kk_sparse_range *x = a, *y = b;
   return (x->start > y->start) - (x->start < y->start);
}

/* The published allocation is immutable. A command's completion protects
 * every version it could acquire, including host update-after-bind changes.
 * Reclamation observes completion; it never waits for it. */
static bool
kk_sparse_publish_ranges(struct kk_device *dev)
{
   struct kk_sparse_device_state *state = kk_sparse_state(dev);
   unsigned count = util_dynarray_num_elements(&state->ranges, struct kk_sparse_range);
   struct kk_sparse_range_version *version = NULL;
   if (count) {
      version = calloc(1, sizeof(*version));
      if (!version) return false;
      version->buffer = mtl_sparse_new_residency_buffer(dev->mtl_handle,
         16 + count * sizeof(struct kk_sparse_range), false);
      if (!version->buffer) { free(version); return false; }
      uint64_t *header = mtl_get_contents(version->buffer);
      header[0] = count;
      memcpy(header + 2, state->ranges.data, state->ranges.size);
      qsort(header + 2, count, sizeof(struct kk_sparse_range), kk_sparse_range_compare);
      kk_sparse_residency(dev, version->buffer, true);
   }
   uint64_t address = version ? mtl_buffer_get_gpu_address(version->buffer) : 0;
   /* A descriptor may be updated while earlier work is pending. Publish a
    * table address only after its allocation is in the committed set. */
   kk_device_make_resources_resident(dev);
   __atomic_store_n((uint64_t *)mtl_get_contents(state->root), address, __ATOMIC_RELEASE);
   if (state->current) {
      state->current->next = state->retired;
      state->retired = state->current;
   }
   state->current = version;
   kk_sparse_reclaim(dev);
   return true;
}

/* Resource creation is the first possible user. Serialize publication with
 * the existing residency lock so an unused device allocates no sparse state. */
static VkResult
kk_sparse_device_ensure(struct kk_device *dev)
{
   if ((!dev->vk.enabled_features.sparseResidencyBuffer &&
        !dev->vk.enabled_features.sparseResidencyImage3D) || kk_sparse_state(dev))
      return VK_SUCCESS;
   simple_mtx_lock(&dev->residency_set.mutex);
   VkResult result = VK_SUCCESS;
   if (!kk_sparse_state(dev)) {
      struct kk_sparse_device_state *state = calloc(1, sizeof(*state));
      if (!state) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto unlock; }
      state->root = mtl_sparse_new_residency_buffer(dev->mtl_handle, 16 + 4096 * 8, false);
      if (!state->root) { free(state); result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto unlock; }
      simple_mtx_init(&state->mutex, mtx_plain);
      state->ranges = UTIL_DYNARRAY_INIT;
      state->retired_maps = UTIL_DYNARRAY_INIT;
      state->next_record = 1; /* zero denotes an ordinary buffer */
      mtl_residency_set_add_allocation(dev->residency_set.handle, state->root);
      dev->residency_set.dirty = true;
      p_atomic_set(&dev->sparse, state);
   }
unlock:
   simple_mtx_unlock(&dev->residency_set.mutex);
   return result;
}

uint32_t
kk_sparse_enabled_modes(struct kk_device *dev)
{
   return (dev->vk.enabled_features.sparseResidencyBuffer ? KK_SPARSE_BUFFER : 0) |
      (dev->vk.enabled_features.sparseResidencyImage3D ? KK_SPARSE_STANDARD_3D : 0);
}

uint32_t
kk_sparse_residency_used(struct kk_device *dev)
{
   return p_atomic_read(&dev->sparse_residency_used);
}

uint64_t
kk_sparse_device_address(struct kk_device *dev)
{
   struct kk_sparse_device_state *state = kk_sparse_state(dev);
   return state ? mtl_buffer_get_gpu_address(state->root) : 0;
}

void
kk_sparse_device_finish(struct kk_device *dev)
{
   struct kk_sparse_device_state *state = kk_sparse_state(dev);
   if (!state) return;
   assert(!state->submissions);
   if (state->current) {
      state->current->next = state->retired;
      state->retired = state->current;
   }
   kk_sparse_reclaim(dev);
   util_dynarray_fini(&state->ranges);
   util_dynarray_fini(&state->retired_maps);
   for (unsigned i = 0; i < 4096; ++i) {
      struct kk_sparse_record_node *mid = state->children[i];
      if (!mid) continue;
      for (unsigned j = 0; j < 1024; ++j) {
         struct kk_sparse_record_node *leaf = mid->children[j];
         if (!leaf) continue;
         kk_sparse_residency(dev, leaf->buffer, false);
         mtl_release(leaf->buffer); free(leaf);
      }
      kk_sparse_residency(dev, mid->buffer, false);
      mtl_release(mid->buffer); free(mid);
   }
   kk_sparse_residency(dev, state->root, false);
   mtl_release(state->root);
   simple_mtx_destroy(&state->mutex);
   free(state); p_atomic_set(&dev->sparse, NULL);
}

static VkResult
kk_sparse_buffer_record(struct kk_device *dev, struct kk_buffer *buffer)
{
   struct kk_sparse_device_state *state = kk_sparse_state(dev);
   /* Opaque binding alone does not require shader residency metadata. */
   if (!dev->vk.enabled_features.sparseResidencyBuffer) return VK_SUCCESS;
   assert(state);
   simple_mtx_lock(&state->mutex);
   if (state->next_record == 0) {
      simple_mtx_unlock(&state->mutex); return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   uint32_t index = state->next_record;
   unsigned hi = index >> 20, middle = (index >> 10) & 1023;
   struct kk_sparse_record_node *mid = state->children[hi];
   if (!mid) {
      mid = calloc(1, sizeof(*mid));
      if (mid) mid->buffer = mtl_sparse_new_residency_buffer(dev->mtl_handle, 1024 * 8, false);
      if (!mid || !mid->buffer) { free(mid); goto fail; }
      state->children[hi] = mid;
      kk_sparse_residency(dev, mid->buffer, true);
      uint64_t *root = mtl_get_contents(state->root);
      root[2 + hi] = mtl_buffer_get_gpu_address(mid->buffer);
   }
   struct kk_sparse_record_node *leaf = mid->children[middle];
   if (!leaf) {
      leaf = calloc(1, sizeof(*leaf));
      if (leaf) leaf->buffer = mtl_sparse_new_residency_buffer(dev->mtl_handle, 1024 * 32, false);
      if (!leaf || !leaf->buffer) { free(leaf); goto fail; }
      mid->children[middle] = leaf;
      kk_sparse_residency(dev, leaf->buffer, true);
      uint64_t *directory = mtl_get_contents(mid->buffer);
      directory[middle] = mtl_buffer_get_gpu_address(leaf->buffer);
   }
   uint64_t *record = (uint64_t *)mtl_get_contents(leaf->buffer) + (index & 1023) * 4;
   record[0] = buffer->vk.device_address;
   record[1] = buffer->vk.device_address + buffer->vk.size;
   record[2] = mtl_buffer_get_gpu_address(buffer->sparse_residency);
   buffer->sparse_record = index;
   state->next_record++;
   struct kk_sparse_range range = {
      .start = record[0], .end = record[1],
      .record_address = mtl_buffer_get_gpu_address(leaf->buffer) + (index & 1023) * 32,
   };
   util_dynarray_append(&state->ranges, range);
   if (!kk_sparse_publish_ranges(dev)) {
      state->ranges.size -= sizeof(range);
      record[1] = record[2] = 0;
      simple_mtx_unlock(&state->mutex);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   simple_mtx_unlock(&state->mutex);
   return VK_SUCCESS;
fail:
   simple_mtx_unlock(&state->mutex);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

bool
kk_sparse_format_properties(struct kk_physical_device *pdev, VkFormat format,
   VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
   VkImageTiling tiling, VkSparseImageFormatProperties *props)
{
   if (!pdev->info.placement_sparse || samples != VK_SAMPLE_COUNT_1_BIT ||
       tiling != VK_IMAGE_TILING_OPTIMAL ||
       (type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_3D) ||
       vk_format_get_plane_count(format) != 1 || vk_format_has_stencil(format))
      return false;
   enum pipe_format pipe = vk_format_to_pipe_format(format);
   const struct kk_va_format *va = kk_get_va_format(pipe);
   if (!va || !va->sparse || (type == VK_IMAGE_TYPE_3D &&
       (vk_format_is_depth_or_stencil(format) || util_format_is_etc(pipe))))
      return false;
   VkFormatFeatureFlags2 features = kk_get_image_format_features(pdev, format, tiling, 0);
   if (!features) return false;
   if (((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(features & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)) ||
       ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) ||
       ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) ||
       ((usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT) != 0))
      return false;
   /* The bridge reports dimensions in texels, including compressed blocks. */
   struct mtl_size tile = mtl_sparse_granularity(pdev->mtl_dev_handle, va->mtl_pixel_format,
      type == VK_IMAGE_TYPE_3D ? MTL_TEXTURE_TYPE_3D : MTL_TEXTURE_TYPE_2D);
   if (!tile.x || !tile.y || !tile.z) return false;
   if (type == VK_IMAGE_TYPE_3D) {
      VkExtent3D shape = kk_standard_3d_shape(pipe);
      if (shape.width) {
         *props = (VkSparseImageFormatProperties){
            .aspectMask = vk_format_aspects(format), .imageGranularity = shape,
         };
         return true;
      }
   }
   unsigned bytes = util_format_get_blocksize(pipe);
   unsigned width = 0, height = 0;
   switch (bytes) {
   case 1: width = 256; height = 256; break;
   case 2: width = 256; height = 128; break;
   case 4: width = 128; height = 128; break;
   case 8: width = 128; height = 64; break;
   case 16: width = 64; height = 64; break;
   }
   bool standard = type == VK_IMAGE_TYPE_2D &&
      tile.x == width * util_format_get_blockwidth(pipe) &&
      tile.y == height * util_format_get_blockheight(pipe) && tile.z == 1;
   *props = (VkSparseImageFormatProperties){
      .aspectMask = vk_format_aspects(format),
      .imageGranularity = {tile.x, tile.y, tile.z},
      .flags = standard ? 0 : VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT,
   };
   return true;
}

bool
kk_sparse_image_format_properties(struct kk_physical_device *pdev, VkFormat format,
   VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags2KHR usage,
   VkImageTiling tiling, VkImageCreateFlags flags,
   VkSparseImageFormatProperties *props)
{
   const VkImageCreateFlags block_flags = VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
      VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
   const bool block_view = type == VK_IMAGE_TYPE_3D &&
      (flags & block_flags) == block_flags &&
      util_format_is_compressed(vk_format_to_pipe_format(format));
   if ((flags & VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT) ||
       ((flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) && !block_view))
      return false;

   if (block_view && (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)) {
      /* Compatible raw views provide their own format usage capabilities.
       * Host transfer remains unavailable for every sparse backing path. */
      if (usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT) return false;
      usage = 0;
   }
   if (!kk_sparse_format_properties(pdev, format,
       kk_sparse_binding_image_type(type, flags), samples, usage, tiling, props))
      return false;
   /* Only the standard-shaped 3D backend supplies block-texel aliases. */
   return !block_view ||
      !(props->flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT);
}

VkResult
kk_sparse_buffer_init(struct kk_device *dev, struct kk_buffer *buffer)
{
   if (!kk_device_physical(dev)->info.placement_sparse)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   VkResult result = kk_sparse_device_ensure(dev);
   if (result != VK_SUCCESS) return result;
   buffer->metal.handle = mtl_new_sparse_buffer(dev->mtl_handle,
      align64(buffer->vk.size, KK_SPARSE_PAGE_SIZE));
   if (!buffer->metal.handle)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   buffer->sparse_residency = mtl_sparse_new_residency_buffer(dev->mtl_handle,
      DIV_ROUND_UP(buffer->vk.size, KK_SPARSE_PAGE_SIZE), false);
   if (!buffer->sparse_residency) {
      mtl_release(buffer->metal.handle);
      buffer->metal.handle = NULL;
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   buffer->vk.device_address = mtl_buffer_get_gpu_address(buffer->metal.handle);
   kk_sparse_residency(dev, buffer->sparse_residency, true);
   kk_sparse_residency(dev, buffer->metal.handle, true);
   result = kk_sparse_buffer_record(dev, buffer);
   if (result != VK_SUCCESS) {
      kk_sparse_buffer_finish(dev, buffer);
      kk_sparse_residency(dev, buffer->metal.handle, false);
      mtl_release(buffer->metal.handle); buffer->metal.handle = NULL;
      return result;
   }
   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT)
      __atomic_fetch_or(&dev->sparse_residency_used, KK_SPARSE_BUFFER, __ATOMIC_RELEASE);
   return VK_SUCCESS;
}

void
kk_sparse_buffer_finish(struct kk_device *dev, struct kk_buffer *buffer)
{
   if (buffer->sparse_residency) {
      if (kk_sparse_state(dev) && buffer->sparse_record) {
         struct kk_sparse_device_state *state = kk_sparse_state(dev);
         simple_mtx_lock(&state->mutex);
         uint32_t index = buffer->sparse_record;
         struct kk_sparse_record_node *leaf = state->children[index >> 20]->children[(index >> 10) & 1023];
         uint64_t *record = (uint64_t *)mtl_get_contents(leaf->buffer) + (index & 1023) * 4;
         /* A retired range can still appear in an older snapshot. Clear its
          * live record so address reuse cannot match a destroyed buffer. */
         record[1] = record[2] = 0;
         util_dynarray_foreach(&state->ranges, struct kk_sparse_range, range) {
            if (range->start != buffer->vk.device_address) continue;
            struct kk_sparse_range *last = util_dynarray_pop_ptr(&state->ranges, struct kk_sparse_range);
            if (range != last) *range = *last;
            break;
         }
         /* Destruction cannot fail. If allocating a smaller snapshot fails,
          * the previous sorted table remains valid with the cleared record. */
         kk_sparse_publish_ranges(dev);
         if (state->submissions) {
            util_dynarray_append(&state->retired_maps, buffer->sparse_residency);
            buffer->sparse_residency = NULL;
         }
         simple_mtx_unlock(&state->mutex);
      }
   }
   if (buffer->sparse_residency) {
      kk_sparse_residency(dev, buffer->sparse_residency, false);
      mtl_release(buffer->sparse_residency);
      buffer->sparse_residency = NULL;
   }
}

struct kk_sparse_view_descriptor
kk_sparse_buffer_view_descriptor(struct kk_buffer_view *view)
{
   struct kk_buffer *buffer = container_of(view->vk.buffer, struct kk_buffer, vk);
   if (!buffer->sparse_residency) return (struct kk_sparse_view_descriptor){0};
   bool rgb32 = view->texel_offset & KK_TEXEL_BUFFER_RGB32;
   uint32_t texel_size = rgb32 ? 4 :
      util_format_get_blocksize(vk_format_to_pipe_format(view->vk.format));
   uint32_t prefix = view->texel_offset & ~KK_TEXEL_BUFFER_RGB32;
   uint64_t first = view->vk.offset - (uint64_t)prefix * texel_size;
   return (struct kk_sparse_view_descriptor){
      .residency_address = mtl_buffer_get_gpu_address(buffer->sparse_residency) + first / KK_SPARSE_PAGE_SIZE,
      .byte_offset = first % KK_SPARSE_PAGE_SIZE,
      .texel_size = texel_size | (rgb32 ? KK_TEXEL_BUFFER_RGB32 : 0),
   };
}

static VkExtent3D
kk_sparse_level_tiles(const struct kk_image *image, uint32_t level)
{
   const VkExtent3D *g = &image->sparse->granularity;
   return (VkExtent3D){
      DIV_ROUND_UP(u_minify(image->vk.extent.width, level), g->width),
      DIV_ROUND_UP(u_minify(image->vk.extent.height, level), g->height),
      DIV_ROUND_UP(u_minify(image->vk.extent.depth, level), g->depth),
   };
}

VkResult
kk_sparse_image_init(struct kk_device *dev, struct kk_image *image,
                     VkImageUsageFlags2KHR declared_usage)
{
   VkSparseImageFormatProperties props;
   if (!kk_sparse_image_format_properties(kk_device_physical(dev), image->vk.format,
       image->vk.image_type, image->vk.samples, declared_usage, image->vk.tiling,
       image->vk.create_flags, &props))
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   if (image->vk.image_type == VK_IMAGE_TYPE_3D &&
       (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) {
      VkResult result = kk_sparse_device_ensure(dev);
      if (result != VK_SUCCESS) return result;
   }
   image->sparse = calloc(1, sizeof(*image->sparse));
   if (!image->sparse) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   struct kk_image_plane *plane = &image->planes[0];
   plane->mtl_handle = mtl_new_sparse_texture(dev->mtl_handle, &plane->layout);
   if (!plane->mtl_handle) {
      free(image->sparse); image->sparse = NULL;
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   image->sparse->aspect_mask = props.aspectMask;
   image->sparse->format_flags = props.flags;
   if (image->vk.image_type == VK_IMAGE_TYPE_3D &&
       (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) {
      VkResult result = kk_standard_3d_image_initialize(dev, image);
      if (result != VK_SUCCESS) {
         kk_standard_3d_image_finish(dev, image);
         mtl_release(plane->mtl_handle); plane->mtl_handle = NULL;
         free(image->sparse); image->sparse = NULL;
         return result;
      }
      kk_sparse_residency(dev, plane->mtl_handle, true);
      __atomic_fetch_or(&dev->sparse_residency_used, KK_SPARSE_STANDARD_3D, __ATOMIC_RELEASE);
      return VK_SUCCESS;
   }
   /* Opaque binding-only images retain their native granularity. */
   if (image->vk.image_type == VK_IMAGE_TYPE_3D) {
      struct mtl_size native = mtl_sparse_granularity(dev->mtl_handle,
         plane->layout.format.mtl, MTL_TEXTURE_TYPE_3D);
      props.imageGranularity = (VkExtent3D){native.x, native.y, native.z};
   }
   image->sparse->granularity = props.imageGranularity;
   image->sparse->tail_first_lod = MIN2(image->vk.mip_levels,
      mtl_texture_first_mipmap_in_tail(plane->mtl_handle));
   image->sparse->tail_pages = image->sparse->tail_first_lod < image->vk.mip_levels ?
      DIV_ROUND_UP(mtl_sparse_tail_size(plane->mtl_handle), KK_SPARSE_PAGE_SIZE) : 0;
   uint64_t pages = 0;
   for (unsigned level = 0; level < image->sparse->tail_first_lod; ++level) {
      image->sparse->level_page[level] = pages;
      VkExtent3D t = kk_sparse_level_tiles(image, level);
      pages += (uint64_t)t.width * t.height * t.depth;
   }
   /* A native multi-page 3D tail must retain one heap across all its pages.
    * Fully resident opaque images expose a bind granule large enough for the
    * tail. Residency images bind the complete tail through its requirement. */
   uint64_t alignment_pages = 1;
   if (!(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT))
      while (alignment_pages < image->sparse->tail_pages) alignment_pages <<= 1;
   image->sparse->level_page[image->sparse->tail_first_lod] = align64(pages, alignment_pages);
   image->sparse->layer_pages = align64(
      image->sparse->level_page[image->sparse->tail_first_lod] + image->sparse->tail_pages,
      alignment_pages);
   plane->layout.size_B = image->sparse->layer_pages * image->vk.array_layers * KK_SPARSE_PAGE_SIZE;
   plane->layout.align_B = alignment_pages * KK_SPARSE_PAGE_SIZE;
   kk_sparse_residency(dev, plane->mtl_handle, true);
   return VK_SUCCESS;
}

void
kk_sparse_image_finish(struct kk_device *dev, struct kk_image *image)
{
   if ((image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) &&
       image->planes[0].mtl_handle)
      kk_sparse_residency(dev, image->planes[0].mtl_handle, false);
   kk_standard_3d_image_finish(dev, image);
   free(image->sparse); image->sparse = NULL;
}

#include "kk_sparse_samplers.h"

struct kk_sparse_view_state {
   struct kk_standard_3d_view_state *standard_3d;
   mtl_texture *query[2];
   mtl_texture *alias_query[2];
   mtl_texture *face_query;
   mtl_buffer *records;
};

VkResult
kk_sparse_image_view_query(struct kk_device *dev, struct kk_image_view *view,
   mtl_texture *parent, const struct kk_view_layout *layout, bool storage)
{
   struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   if (!(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) ||
       (storage && !view->vk.base_mip_level && !image->sparse->standard_3d))
      return VK_SUCCESS;
   if (!view->sparse) {
      view->sparse = vk_zalloc(&dev->vk.alloc, sizeof(*view->sparse), 8,
                               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!view->sparse) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      view->sparse->records = mtl_sparse_new_residency_buffer(dev->mtl_handle,
         kk_standard_3d_query_bytes(image), false);
      if (!view->sparse->records) return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      kk_sparse_residency(dev, view->sparse->records, true);
   }
   struct kk_view_layout query_layout = *layout;
   query_layout.base_level = 0;
   bool logical_raw=image->sparse->standard_3d && util_format_is_compressed(image->planes[0].layout.format.pipe) && !util_format_is_compressed(layout->format.pipe);
   query_layout.num_levels = logical_raw ? view->vk.level_count : image->vk.mip_levels;
   query_layout.min_lod_clamp = 0;
   view->sparse->query[storage] = mtl_new_texture_view_with(parent, &query_layout);
   if (!view->sparse->query[storage]) return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   struct kk_sparse_query_record *record = (void *)((char *)
      mtl_get_contents(view->sparse->records) + storage * kk_standard_3d_query_stride(view));
   (*record) = (struct kk_sparse_query_record){
      .resource_id = mtl_texture_get_gpu_resource_id(view->sparse->query[storage]),
      .array_query_resource_id = mtl_texture_get_gpu_resource_id(view->sparse->query[storage]),
      .base_lod = view->vk.base_mip_level, .level_count = view->vk.level_count,
      .view_type = view->vk.view_type,
   };
   VkResult logical = kk_standard_3d_query_initialize(view, storage, layout);
   if (logical != VK_SUCCESS) return logical;
   if (storage) {
      /* The sliced-3D lowering reads the same descriptor prefix after the
       * sparse load has selected this parent view. Keep its range at +8. */
      (*record).storage_slices.offset = view->vk.storage.z_slice_offset;
      (*record).storage_slices.count = view->vk.storage.z_slice_count;
      (*record).storage_base_lod = view->vk.base_mip_level;
      (*record).storage_level_count = view->vk.level_count;
   }
   if (!storage) {
      VkResult result = kk_sparse_sample_row(dev, view->vk.base_mip_level,
         view->vk.level_count, &(*record).sampler_table);
      if (result != VK_SUCCESS) return result;
      for (unsigned i = 0; i < 3; ++i) {
         unsigned extent = i == 0 ? image->vk.extent.width : i == 1 ? image->vk.extent.height : image->vk.extent.depth;
         unsigned view_size = logical_raw ? (i==0?view->vk.extent.width:i==1?view->vk.extent.height:view->vk.extent.depth) : MAX2(extent >> view->vk.base_mip_level, 1);
         (*record).view_size[i] = view_size;
         (*record).gradient_scale[i] = (float)((uint64_t)view_size << view->vk.base_mip_level) / extent;
      }
      if (view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE || view->vk.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
         query_layout.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
         view->sparse->face_query = mtl_new_texture_view_with(parent, &query_layout);
         if (!view->sparse->face_query) return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         (*record).face_resource_id = mtl_texture_get_gpu_resource_id(view->sparse->face_query);
      }
   }
   return VK_SUCCESS;
}

VkResult
kk_sparse_image_view_query_aliases(struct kk_device *dev,
   struct kk_image_view *view, mtl_texture *parent,
   const struct kk_view_layout *layout, bool storage, bool view_is_array)
{
   /* The alias owner calls this only for its eligible original 1D/2D views.
    * The original parent avoids applying a view's base layer a second time. */
   if (!view->sparse || !view->sparse->query[storage] ||
       view->sparse->alias_query[storage])
      return VK_SUCCESS;
   assert(view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
          view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
   struct kk_view_layout alias = *layout;
   alias.base_level = 0;
   alias.num_levels = view->vk.image->mip_levels;
   alias.min_lod_clamp = 0;
   alias.view_type = view_is_array ? VK_IMAGE_VIEW_TYPE_2D :
                                    VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   mtl_texture *alternate = mtl_new_texture_view_with(parent, &alias);
   if (!alternate) return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   view->sparse->alias_query[storage] = alternate;
   struct kk_sparse_query_record *record =
      (void *)((char *)mtl_get_contents(view->sparse->records) + storage * kk_standard_3d_query_stride(view));
   uint64_t alternate_id = mtl_texture_get_gpu_resource_id(alternate);
   if (view_is_array) {
      record->array_query_resource_id = record->resource_id;
      record->resource_id = alternate_id;
   } else {
      record->array_query_resource_id = alternate_id;
   }
   return VK_SUCCESS;
}

void
kk_sparse_storage_descriptor_aliases(const struct kk_image_view *view,
   uint64_t plain_id, uint64_t array_id)
{
   if (!view->sparse || !view->sparse->query[true]) return;
   struct kk_sparse_query_record *record =
      (void *)((char *)mtl_get_contents(view->sparse->records) + kk_standard_3d_query_stride(view));
   /* All descriptor writers for an immutable view publish the same IDs. */
   __atomic_store_n(&record->plain_resource_id, plain_id, __ATOMIC_RELAXED);
   __atomic_store_n(&record->array_resource_id, array_id, __ATOMIC_RELAXED);
}

uint64_t
kk_sparse_image_view_query_address(struct kk_image_view *view, bool storage)
{
   return view->sparse && view->sparse->query[storage] ?
      mtl_buffer_get_gpu_address(view->sparse->records) +
      storage * kk_standard_3d_query_stride(view) : 0;
}

void
kk_sparse_storage_descriptor(struct kk_storage_image_descriptor *desc,
                             struct kk_image_view *view)
{
   uint64_t query = kk_sparse_image_view_query_address(view, true);
   if (!query) return;
   struct kk_sparse_query_record *record = (void *)(
      (char *)mtl_get_contents(view->sparse->records) + kk_standard_3d_query_stride(view));
   /* Writers may run concurrently for the same immutable view. */
   __atomic_store_n(&record->storage_slices.offset,
                    view->vk.storage.z_slice_offset, __ATOMIC_RELAXED);
   __atomic_store_n(&record->storage_slices.count,
                    view->vk.storage.z_slice_count, __ATOMIC_RELAXED);
   if (view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D ||
       view->vk.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
       view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D ||
       view->vk.view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
      /* Without an alias both fields retain the actual view's native ID.
       * The alias producer has already supplied both typed IDs otherwise. */
      uint64_t empty = 0;
      __atomic_compare_exchange_n(&record->plain_resource_id, &empty,
         desc->image_gpu_resource_id, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
      empty = 0;
      __atomic_compare_exchange_n(&record->array_resource_id, &empty,
         desc->image_gpu_resource_id, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
      desc->image_gpu_resource_id = 0;
   }
   uint64_t encoded = kk_storage_sparse_query_encode(query);
   memcpy(&desc->texel_offset, &encoded, sizeof(encoded));
}

void
kk_sparse_image_view_finish(struct kk_device *dev, struct kk_image_view *view)
{
   if (!view->sparse) return;
   kk_standard_3d_query_finish(dev, view);
   for (unsigned i = 0; i < 2; ++i) {
      mtl_release(view->sparse->query[i]);
      mtl_release(view->sparse->alias_query[i]);
   }
   mtl_release(view->sparse->face_query);
   if (view->sparse->records) {
      kk_sparse_residency(dev, view->sparse->records, false);
      mtl_release(view->sparse->records);
   }
   vk_free(&dev->vk.alloc, view->sparse);
   view->sparse = NULL;
}

void
kk_sparse_image_requirements(struct kk_image *image, uint32_t *count,
                             VkSparseImageMemoryRequirements2 *requirements)
{
   if (!(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) {
      *count = 0;
      return;
   }
   if (!requirements) { *count = 1; return; }
   if (!*count) return;
   *count = 1;
   const VkSparseImageFormatProperties props = {
      .aspectMask = image->sparse->aspect_mask,
      .imageGranularity = image->sparse->granularity,
      .flags = image->sparse->format_flags,
   };
   requirements->memoryRequirements = (VkSparseImageMemoryRequirements){
      .formatProperties = props,
      .imageMipTailFirstLod = image->sparse->tail_first_lod,
      .imageMipTailSize = image->sparse->tail_pages * KK_SPARSE_PAGE_SIZE,
      .imageMipTailOffset = image->sparse->level_page[image->sparse->tail_first_lod] * KK_SPARSE_PAGE_SIZE,
      .imageMipTailStride = image->sparse->layer_pages * KK_SPARSE_PAGE_SIZE,
   };
}

static mtl_heap *
kk_sparse_heap(VkDeviceMemory memory)
{
   VK_FROM_HANDLE(kk_device_memory, mem, memory);
   return mem ? mem->bo->mtl_handle : NULL;
}

struct kk_sparse_mapping_commit {
   struct kk_queue *queue;
   mtl_command_buffer *command;
   mtl_command_allocator *allocator;
   struct util_dynarray copies;
};

static void
kk_sparse_release_copies(struct kk_device *dev, struct util_dynarray *copies)
{
   util_dynarray_foreach(copies, struct mtl_sparse_residency_copy, copy) {
      kk_sparse_residency(dev, copy->source, false);
      mtl_release(copy->source);
   }
   util_dynarray_fini(copies);
}

static void
kk_sparse_mapping_complete(struct mtl_feedback_data *feedback)
{
   struct kk_sparse_mapping_commit *commit = feedback->user_data;
   struct kk_queue *queue = commit->queue;
   struct kk_device *dev = kk_queue_device(queue);
   if (feedback->error != MTL_COMMAND_QUEUE_ERROR_NONE)
      vk_device_set_lost(&dev->vk, "Sparse mapping barrier: %s", feedback->error_message);
   mtl_release(commit->command);
   mtl_release(commit->allocator);
   kk_sparse_release_copies(dev, &commit->copies);
   mtx_lock(&queue->mutex);
   if (--queue->commits_in_flight == 0) {
      kk_ResetCommandPool(kk_device_to_handle(dev), kk_cmd_pool_to_handle(queue->cmd_pool), 0);
      cnd_broadcast(&queue->cond);
   }
   mtx_unlock(&queue->mutex);
   vk_free(&dev->vk.alloc, commit);
}

static VkResult
kk_sparse_publish_mappings(struct kk_queue *queue, struct util_dynarray *copies)
{
   struct kk_device *dev = kk_queue_device(queue);
   struct kk_sparse_mapping_commit *commit = vk_zalloc(&dev->vk.alloc,
      sizeof(*commit), 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!commit) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   commit->queue = queue;
   commit->copies = *copies;
   kk_device_make_resources_resident(dev);
   mtx_lock(&queue->mutex);
   ++queue->commits_in_flight;
   mtx_unlock(&queue->mutex);
   if (mtl_sparse_commit_mapping_barrier(dev->mtl_handle, queue->mtl_handle,
       &commit->command, &commit->allocator, copies->data,
       util_dynarray_num_elements(copies, struct mtl_sparse_residency_copy),
       kk_sparse_mapping_complete, commit)) {
      *copies = (struct util_dynarray){0};
      return VK_SUCCESS;
   }
   mtx_lock(&queue->mutex);
   if (--queue->commits_in_flight == 0) cnd_broadcast(&queue->cond);
   mtx_unlock(&queue->mutex);
   vk_free(&dev->vk.alloc, commit);
   return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

static bool
kk_sparse_memory_valid(VkDeviceMemory memory)
{
   VK_FROM_HANDLE(kk_device_memory, mem, memory);
   if (!mem) return true;
   struct kk_physical_device *pdev = kk_device_physical((struct kk_device *)mem->vk.base.device);
   return mem->bo->mtl_handle && !mem->vk.import_handle_type &&
      !(pdev->mem_types[mem->vk.memory_type_index].propertyFlags &
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT));
}

struct kk_sparse_texture_batch {
   mtl_command_queue *queue;
   mtl_texture *texture;
   mtl_heap *heap;
   struct util_dynarray ops;
};

static void
kk_sparse_flush_textures(struct kk_sparse_texture_batch *batch)
{
   if (batch->ops.size)
      mtl_sparse_map_texture_batch(batch->queue, batch->texture, batch->heap,
         batch->ops.data, util_dynarray_num_elements(&batch->ops, struct mtl_sparse_texture_mapping));
   util_dynarray_clear(&batch->ops);
}

static bool
kk_sparse_append_texture(struct kk_sparse_texture_batch *batch, mtl_texture *texture,
   mtl_heap *heap, struct mtl_sparse_texture_mapping op)
{
   if (batch->texture != texture || batch->heap != heap) {
      kk_sparse_flush_textures(batch);
      batch->texture = texture;
      batch->heap = heap;
   }
   struct mtl_sparse_texture_mapping *slot = util_dynarray_grow(&batch->ops, struct mtl_sparse_texture_mapping, 1);
   if (!slot) return false;
   *slot = op;
   return true;
}

#include "kk_standard_sparse_3d_impl.h"

static bool
kk_sparse_opaque_bind(struct kk_sparse_texture_batch *batch, struct kk_image *image, const VkSparseMemoryBind *bind)
{
   const struct kk_sparse_image *s = image->sparse;
   uint64_t first = bind->resourceOffset / KK_SPARSE_PAGE_SIZE;
   uint64_t end = first + DIV_ROUND_UP(bind->size, KK_SPARSE_PAGE_SIZE);
   uint64_t heap_page = bind->memoryOffset / KK_SPARSE_PAGE_SIZE;
   uint64_t tail = s->level_page[s->tail_first_lod], regular = 0;
   if (s->tail_first_lod) {
      VkExtent3D last = kk_sparse_level_tiles(image, s->tail_first_lod - 1);
      regular = s->level_page[s->tail_first_lod - 1] +
         (uint64_t)last.width * last.height * last.depth;
   }
   /* Emit whole contiguous rows, including a partial row at either end. */
   while (first < end) {
      uint32_t layer = first / s->layer_pages;
      uint64_t page = first % s->layer_pages;
      uint32_t level = 0;
      struct mtl_size origin = {0}, size = {1, 1, 1};
      bool padding = false;
      if (page >= tail + s->tail_pages) {
         padding = true;
         size.x = MIN2(end - first, s->layer_pages - page);
      } else if (page >= regular && page < tail) {
         padding = true;
         size.x = MIN2(end - first, tail - page);
      } else if (page >= tail) {
         level = s->tail_first_lod;
         origin.x = page - tail;
         size.x = MIN2(end - first, tail + s->tail_pages - page);
      } else {
         while (level + 1 < s->tail_first_lod && s->level_page[level + 1] <= page) ++level;
         VkExtent3D t = kk_sparse_level_tiles(image, level);
         uint64_t local = page - s->level_page[level];
         origin = (struct mtl_size){local % t.width, (local / t.width) % t.height,
                                    local / ((uint64_t)t.width * t.height)};
         size.x = MIN2(end - first, t.width - origin.x);
      }
      struct mtl_sparse_texture_mapping op = {
         .mode = bind->memory ? 0 : 1, .origin = origin, .size = size,
         .level = level, .layer = layer, .offset = heap_page,
      };
      if (!padding && !kk_sparse_append_texture(batch, image->planes[0].mtl_handle,
                                   kk_sparse_heap(bind->memory), op))
         return false;
      first += size.x;
      heap_page += size.x;
   }
   return true;
}

VkResult
kk_queue_bind_sparse(struct kk_queue *queue, struct vk_queue_submit *submit)
{
   struct kk_device *dev = kk_queue_device(queue);
   /* Check memory before changing mappings. Imported host buffers have no heap. */
   for (unsigned i = 0; i < submit->buffer_bind_count; ++i)
      for (unsigned j = 0; j < submit->buffer_binds[i].bindCount; ++j)
         if (!kk_sparse_memory_valid(submit->buffer_binds[i].pBinds[j].memory))
            return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   for (unsigned i = 0; i < submit->image_opaque_bind_count; ++i)
      for (unsigned j = 0; j < submit->image_opaque_binds[i].bindCount; ++j)
         if (!kk_sparse_memory_valid(submit->image_opaque_binds[i].pBinds[j].memory))
            return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   for (unsigned i = 0; i < submit->image_bind_count; ++i)
      for (unsigned j = 0; j < submit->image_binds[i].bindCount; ++j)
         if (!kk_sparse_memory_valid(submit->image_binds[i].pBinds[j].memory))
            return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);

   VkResult result = VK_SUCCESS;
   struct util_dynarray buffer_ops = UTIL_DYNARRAY_INIT;
   struct util_dynarray copies = UTIL_DYNARRAY_INIT;
   struct kk_sparse_texture_batch batch = {.queue = queue->mtl_handle, .ops = UTIL_DYNARRAY_INIT};
   for (unsigned i = 0; i < submit->buffer_bind_count; ++i) {
      const VkSparseBufferMemoryBindInfo *info = &submit->buffer_binds[i];
      VK_FROM_HANDLE(kk_buffer, buffer, info->buffer);
      for (unsigned j = 0; j < info->bindCount;) {
         VkDeviceMemory memory = info->pBinds[j].memory;
         unsigned end = j;
         while (end < info->bindCount && info->pBinds[end].memory == memory) ++end;
         util_dynarray_clear(&buffer_ops);
         for (; j < end; ++j) {
            const VkSparseMemoryBind *b = &info->pBinds[j];
            struct mtl_sparse_buffer_mapping *op = util_dynarray_grow(&buffer_ops, struct mtl_sparse_buffer_mapping, 1);
            if (!op) { result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY); goto finish; }
            *op = (struct mtl_sparse_buffer_mapping){.mode = memory ? 0 : 1,
               .first = b->resourceOffset / KK_SPARSE_PAGE_SIZE,
               .count = DIV_ROUND_UP(b->size, KK_SPARSE_PAGE_SIZE),
               .offset = b->memoryOffset / KK_SPARSE_PAGE_SIZE};
            struct mtl_sparse_residency_copy *copy = util_dynarray_grow(&copies, struct mtl_sparse_residency_copy, 1);
            if (!copy) { result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY); goto finish; }
            *copy = (struct mtl_sparse_residency_copy){
               .destination = buffer->sparse_residency,
               .destination_offset = op->first, .size = op->count,
            };
            copy->source = mtl_sparse_new_residency_buffer(dev->mtl_handle, op->count, memory != VK_NULL_HANDLE);
            if (!copy->source) { copies.size -= sizeof(*copy); result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY); goto finish; }
            kk_sparse_residency(dev, copy->source, true);
         }
         mtl_sparse_map_buffer_batch(queue->mtl_handle, buffer->metal.handle, kk_sparse_heap(memory),
            buffer_ops.data, util_dynarray_num_elements(&buffer_ops, struct mtl_sparse_buffer_mapping));
      }
   }
   for (unsigned i = 0; i < submit->image_opaque_bind_count; ++i) {
      const VkSparseImageOpaqueMemoryBindInfo *info = &submit->image_opaque_binds[i];
      VK_FROM_HANDLE(kk_image, image, info->image);
      if (image->sparse->standard_3d) {
         simple_mtx_lock(&image->sparse->standard_3d->mutex);
         bool ok = kk_standard_3d_bind_opaque(queue, &batch, &copies, image, info->bindCount, info->pBinds);
         simple_mtx_unlock(&image->sparse->standard_3d->mutex);
         if (!ok) {
            result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY); goto finish;
         }
         continue;
      }
      for (unsigned j = 0; j < info->bindCount; ++j)
         if (!kk_sparse_opaque_bind(&batch, image, &info->pBinds[j])) {
            result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY); goto finish;
         }
   }
   for (unsigned i = 0; i < submit->image_bind_count; ++i) {
      const VkSparseImageMemoryBindInfo *info = &submit->image_binds[i];
      VK_FROM_HANDLE(kk_image, image, info->image);
      if (image->sparse->standard_3d) {
         simple_mtx_lock(&image->sparse->standard_3d->mutex);
         bool ok = kk_standard_3d_bind_images(queue, &batch, &copies, image, info->bindCount, info->pBinds);
         simple_mtx_unlock(&image->sparse->standard_3d->mutex);
         if (!ok) {
            result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY); goto finish;
         }
         continue;
      }
      const VkExtent3D *g = &image->sparse->granularity;
      for (unsigned j = 0; j < info->bindCount; ++j) {
         const VkSparseImageMemoryBind *b = &info->pBinds[j];
         struct mtl_size origin = {b->offset.x / g->width, b->offset.y / g->height, b->offset.z / g->depth};
         struct mtl_size size = {DIV_ROUND_UP(b->extent.width, g->width),
            DIV_ROUND_UP(b->extent.height, g->height), DIV_ROUND_UP(b->extent.depth, g->depth)};
         struct mtl_sparse_texture_mapping op = {.mode = b->memory ? 0 : 1,
            .origin = origin, .size = size, .level = b->subresource.mipLevel,
            .layer = b->subresource.arrayLayer, .offset = b->memoryOffset / KK_SPARSE_PAGE_SIZE};
         if (!kk_sparse_append_texture(&batch, image->planes[0].mtl_handle, kk_sparse_heap(b->memory), op)) {
            result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY); goto finish;
         }
      }
   }
   kk_sparse_flush_textures(&batch);
   result = kk_sparse_publish_mappings(queue, &copies);
finish:
   kk_sparse_release_copies(dev, &copies);
   util_dynarray_fini(&buffer_ops);
   util_dynarray_fini(&batch.ops);
   return result;
}
