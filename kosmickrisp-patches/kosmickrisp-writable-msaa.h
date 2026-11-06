/* SPDX-License-Identifier: MIT */
#ifndef KK_WRITABLE_MSAA_H
#define KK_WRITABLE_MSAA_H

#include "kk-writable-msaa-layout.h"
#include "kk_image.h"
#include "kk_image_view.h"
#include "kk_descriptor_types.h"
#include "kk_format.h"
#include "util/format/u_format.h"

#define KK_WRITABLE_MSAA_MAX_SAMPLES 4

struct kk_writable_msaa_image {
   struct kk_image_layout sample_layout;
   uint64_t sample_offset_B;
   uint64_t sample_stride_B;
   uint64_t total_size_B;
   uint64_t align_B;
   mtl_texture *sample[KK_WRITABLE_MSAA_MAX_SAMPLES];
   VkImage native_image;
};

struct kk_writable_msaa_view {
   struct kk_bo *table;
   uint32_t sample_count;
   mtl_texture *sample[KK_WRITABLE_MSAA_MAX_SAMPLES];
};

static inline bool
kk_writable_msaa_requested(const struct vk_image *image)
{
   return image->samples > VK_SAMPLE_COUNT_1_BIT &&
          (image->usage & VK_IMAGE_USAGE_STORAGE_BIT);
}

/* Metal permits color views with the same texel bit length. Integer raw views
 * preserve NaNs, signed-normalized endpoints and packed fields in both copy
 * directions; floating-point sampling and attachment conversion would not.
 */
static inline enum pipe_format
kk_writable_msaa_raw_format(enum pipe_format format)
{
   switch (util_format_get_blocksize(kk_format_backing(format))) {
   case 1: return PIPE_FORMAT_R8_UINT;
   case 2: return PIPE_FORMAT_R16_UINT;
   case 4: return PIPE_FORMAT_R32_UINT;
   case 8: return PIPE_FORMAT_R32G32_UINT;
   case 16: return PIPE_FORMAT_R32G32B32A32_UINT;
   default: UNREACHABLE("Invalid writable multisample color format");
   }
}

VkResult kk_writable_msaa_image_init(struct kk_device *dev,
   struct kk_image *image, VkImageLayout initial_layout);
void kk_writable_msaa_image_finish(struct kk_device *dev, struct kk_image *image);
VkResult kk_writable_msaa_image_bind(struct kk_device *dev, struct kk_image *image);
VkResult kk_writable_msaa_view_init(struct kk_device *dev, struct kk_image_view *view);
void kk_writable_msaa_view_finish(struct kk_device *dev, struct kk_image_view *view);

void kk_writable_msaa_storage_descriptor(struct kk_storage_image_descriptor *desc,
                                        const struct kk_image_view *view);
void kk_writable_msaa_sampled_descriptor(struct kk_sampled_image_descriptor *desc,
   const struct kk_image_view *view, bool input_attachment);

struct kk_cmd_buffer;
void kk_writable_msaa_prepare_render(struct kk_cmd_buffer *cmd);
void kk_writable_msaa_publish_render(struct kk_cmd_buffer *cmd);
void kk_writable_msaa_record_draw(struct kk_cmd_buffer *cmd);
bool kk_writable_msaa_copy_image(struct kk_cmd_buffer *cmd,
                                const VkCopyImageInfo2 *copy);

#endif
