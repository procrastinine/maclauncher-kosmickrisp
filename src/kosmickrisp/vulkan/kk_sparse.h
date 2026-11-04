/* SPDX-License-Identifier: MIT */
#ifndef KK_SPARSE_H
#define KK_SPARSE_H
#include "vk_image.h"
#include "kk_descriptor_types.h"
#include "kosmickrisp/compiler/kk_sparse_layout.h"
#include "kosmickrisp/bridge/mtl_types.h"
static_assert(sizeof(struct kk_sampled_image_descriptor) == 64, "sampled descriptor ABI");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_uint16) == 20, "integer image LOD ABI");
static_assert(offsetof(struct kk_sampled_image_descriptor, image_min_lod_fp16) == 18, "fp16 image LOD ABI");
#define KK_SPARSE_PAGE_SIZE 65536ull
#define KK_STORAGE_SPARSE_QUERY_TAG (UINT64_C(1) << 63)
#define KK_STORAGE_MAX_3D_DIMENSION 2048u
static_assert(KK_STORAGE_MAX_3D_DIMENSION < (1u << 31), "slice count cannot set the sparse tag");
static_assert(sizeof(struct kk_storage_image_descriptor) == 16, "storage descriptor ABI");
static_assert(offsetof(struct kk_storage_image_descriptor, texel_offset) == 8, "storage slice offset ABI");
static_assert(offsetof(struct kk_storage_image_descriptor, texel_count) == 12, "storage slice count ABI");

static inline uint64_t
kk_storage_sparse_query_encode(uint64_t address)
{
   assert(!(address & KK_STORAGE_SPARSE_QUERY_TAG));
   return address ? address | KK_STORAGE_SPARSE_QUERY_TAG : 0;
}
static inline uint64_t
kk_storage_sparse_query_decode(uint64_t metadata)
{
   return metadata & KK_STORAGE_SPARSE_QUERY_TAG ?
      metadata & ~KK_STORAGE_SPARSE_QUERY_TAG : 0;
}
/* Opaque native IDs own all their bits. Only the zero-primary form can
 * identify an indirect 1D/2D descriptor; an all-zero descriptor stays null. */
static inline uint64_t
kk_storage_sparse_descriptor_query(uint64_t primary, uint64_t metadata)
{
   return primary == 0 && metadata != 0 ?
      kk_storage_sparse_query_decode(metadata) : 0;
}
struct nir_builder;
struct nir_def;
/* Caller supplies a validated record pointer, never an opaque native ID.
 * Address zero reads no memory and returns the native kind. */
struct nir_def *kk_nir_query_record_kind(struct nir_builder *b,
                                       struct nir_def *record_address);
struct nir_def *kk_nir_storage_sparse_query_decode(struct nir_builder *b,
                                                 struct nir_def *metadata);
struct nir_def *kk_nir_storage_sparse_slice_range(struct nir_builder *b,
                                                struct nir_def *range);
struct nir_def *kk_nir_storage_sparse_descriptor_query(struct nir_builder *b,
                                                      struct nir_def *descriptor);
struct nir_def *kk_nir_storage_sparse_handle_address(struct nir_builder *b,
   struct nir_def *descriptor_address, struct nir_def *descriptor, bool array);
static inline VkImageType
kk_sparse_binding_image_type(VkImageType type, VkImageCreateFlags flags)
{
   /* Share the driver's existing 1D-as-2D representation for opaque binding.
    * Vulkan has no partially resident 1D image type. */
   return type == VK_IMAGE_TYPE_1D && !(flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)
      ? VK_IMAGE_TYPE_2D : type;
}
struct kk_device_memory;
struct kk_device;
void kk_standard_3d_memory_finish(struct kk_device *,struct kk_device_memory *);
struct kk_physical_device;
struct kk_image;
struct kk_buffer;
struct kk_buffer_view;
struct kk_image_view;
struct kk_view_layout;
struct kk_sparse_view_state;
struct kk_sparse_device_state;
struct kk_image_view;
struct kk_view_layout;
VkResult kk_standard_3d_raw_view(struct kk_device *,struct kk_image_view *,const struct kk_view_layout *,mtl_texture **);
struct kk_queue;
struct kk_cmd_buffer;
void kk_standard_3d_blit(struct kk_cmd_buffer *,const VkBlitImageInfo2 *);
void kk_standard_3d_clear(struct kk_cmd_buffer *,struct kk_image *,VkImageLayout,const VkClearColorValue *,uint32_t,const VkImageSubresourceRange *);
void kk_standard_3d_copy_buffer_to_image(struct kk_cmd_buffer *,const VkCopyBufferToImageInfo2 *);
void kk_standard_3d_copy_image_to_buffer(struct kk_cmd_buffer *,const VkCopyImageToBufferInfo2 *);
void kk_standard_3d_copy_image(struct kk_cmd_buffer *,const VkCopyImageInfo2 *);
struct kk_rc_sampler;
struct vk_queue_submit;
/* Appended to the existing texel-buffer descriptor. Ordinary image
 * descriptors keep their existing size and layout. */
struct kk_sparse_view_descriptor {
   uint64_t residency_address;
   uint32_t byte_offset, texel_size;
};
struct kk_standard_3d_image;
struct kk_sparse_image {
   struct kk_standard_3d_image *standard_3d;
   VkExtent3D granularity;
   VkImageAspectFlags aspect_mask;
   VkSparseImageFormatFlags format_flags;
   uint32_t tail_first_lod;
   uint64_t tail_pages;
   uint64_t level_page[17];
   uint64_t layer_pages;
};
bool kk_sparse_format_properties(struct kk_physical_device *pdev, VkFormat format,
   VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
   VkImageTiling tiling, VkSparseImageFormatProperties *props);
bool kk_sparse_image_format_properties(struct kk_physical_device *pdev, VkFormat format,
   VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags2KHR usage,
   VkImageTiling tiling, VkImageCreateFlags flags,
   VkSparseImageFormatProperties *props);
uint32_t kk_sparse_memory_type_bits(struct kk_physical_device *pdev);
VkResult kk_sparse_image_init(struct kk_device *dev, struct kk_image *image,
                             VkImageUsageFlags2KHR declared_usage);
void kk_sparse_image_finish(struct kk_device *dev, struct kk_image *image);
VkResult kk_sparse_image_view_query(struct kk_device *dev, struct kk_image_view *view,
   mtl_texture *parent, const struct kk_view_layout *layout, bool storage);
VkResult kk_sparse_image_view_query_aliases(struct kk_device *dev,
   struct kk_image_view *view, mtl_texture *parent,
   const struct kk_view_layout *layout, bool storage, bool view_is_array);
void kk_sparse_storage_descriptor_aliases(const struct kk_image_view *view,
   uint64_t plain_id, uint64_t array_id);
void kk_sparse_image_view_finish(struct kk_device *dev, struct kk_image_view *view);
uint64_t kk_sparse_image_view_query_address(struct kk_image_view *view, bool storage);
void kk_sparse_storage_descriptor(struct kk_storage_image_descriptor *desc,
                                  struct kk_image_view *view);
VkResult kk_sparse_buffer_init(struct kk_device *dev, struct kk_buffer *buffer);
enum kk_sparse_mode {
   KK_SPARSE_BUFFER = 1u << 0,
   KK_SPARSE_STANDARD_3D = 1u << 1,
   KK_SPARSE_MODES = KK_SPARSE_BUFFER | KK_SPARSE_STANDARD_3D,
};
uint32_t kk_sparse_enabled_modes(struct kk_device *dev);
uint32_t kk_sparse_residency_used(struct kk_device *dev);
VkResult kk_sparse_sampler_added(struct kk_device *dev, struct kk_rc_sampler *sampler);
void kk_sparse_sampler_removed(struct kk_device *dev, struct kk_rc_sampler *sampler);
void kk_sparse_sample_rows_finish(struct kk_device *dev);
void kk_sparse_device_finish(struct kk_device *dev);
uint64_t kk_sparse_device_address(struct kk_device *dev);
VkResult kk_sparse_prepare_pipeline(struct kk_device *dev, uint32_t modes);
bool kk_sparse_submission_begin(struct kk_device *dev);
void kk_sparse_submission_end(struct kk_device *dev);
void kk_sparse_buffer_finish(struct kk_device *dev, struct kk_buffer *buffer);
/* Borrow the actual selected virtual source; this builder does not retain it.
 * first_byte is buffer-relative. coordinate_stride_B advances one incoming
 * residency coordinate; rgb32_scalar covers three consecutive 4-byte native
 * components. Raw logical RGB32 uses stride 12 and rgb32_scalar=false. */
struct kk_sparse_view_descriptor kk_sparse_buffer_range_descriptor(
   const struct kk_buffer *buffer, uint64_t first_byte,
   uint32_t coordinate_stride_B, bool rgb32_scalar);
struct kk_sparse_view_descriptor kk_sparse_buffer_view_descriptor(
   struct kk_buffer_view *view);
void kk_sparse_image_requirements(struct kk_image *image, uint32_t *count,
   VkSparseImageMemoryRequirements2 *requirements);
VkResult kk_queue_bind_sparse(struct kk_queue *queue, struct vk_queue_submit *submit);
#endif
