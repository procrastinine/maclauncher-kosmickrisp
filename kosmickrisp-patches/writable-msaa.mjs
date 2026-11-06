// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'writable-msaa';
const headers = ['kk-writable-msaa.h', 'kk-writable-msaa-layout.h',
                 'kk-writable-msaa-image.h',
                 'kk-writable-msaa-view.h', 'kk-writable-msaa-nir.h',
                 'kk-writable-msaa-meta.h'];
export const sourceFiles = headers.map(x => 'kosmickrisp-patches/' + x.replace(/^kk-/, 'kosmickrisp-'));
export function apply({edit, edits}) {
  const change = (file, from, to) => edit(file, s => {
    const count = s.split(from).length - 1;
    if (count !== 1) throw Error(`${id}: ${file}: expected one anchor, found ${count}: ${from.slice(0,80)}`);
    return s.replace(from, to);
  });
  const kk = (file, from, to) => change('src/kosmickrisp/vulkan/' + file, from, to);
  for (const header of headers) {
    const source = header.replace(/^kk-/, 'kosmickrisp-');
    edits.set('src/kosmickrisp/vulkan/' + header,
      fs.readFileSync(new URL('./' + source, import.meta.url), 'utf8'));
  }
  kk('kk_physical_device.c', '      .shaderStorageImageExtendedFormats = true,',
     '      .shaderStorageImageMultisample = true,\n      .shaderStorageImageExtendedFormats = true,');
  kk('kk_physical_device.c', '      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,',
     `      .storageImageSampleCounts = pdev->info.supported_sample_counts &
         (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT),`);
  kk('kk_image.c', `       !(create_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       !(all_usage & VK_IMAGE_USAGE_STORAGE_BIT)) {`,
     `       !(create_flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       (!(all_usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        !(create_flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                          VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                          VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)))) {`);
  kk('kk_image.c', `         sampleCounts = pdev->info.supported_sample_counts;
   }`,
     `         sampleCounts = pdev->info.supported_sample_counts;
   }
   if (all_usage & VK_IMAGE_USAGE_STORAGE_BIT)
      sampleCounts &= VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT |
                      VK_SAMPLE_COUNT_4_BIT;`);
  kk('kk_image.h', '   struct vk_image vk;',
     '   struct vk_image vk;\n   struct kk_writable_msaa_image *writable_msaa;');
  kk('kk_image_view.h', '   struct vk_image_view vk;',
     '   struct vk_image_view vk;\n   struct kk_writable_msaa_view *writable_msaa;');
  kk('kk_image.c', '#include "kk_image.h"',
     '#include "kk_image.h"\n#include "kk-writable-msaa.h"');
  kk('kk_image.c', '   vk_image_init(&dev->vk, &image->vk, pCreateInfo);',
     '   image->writable_msaa = NULL;\n   vk_image_init(&dev->vk, &image->vk, pCreateInfo);');
  kk('kk_image.c',
     '   const struct vk_format_ycbcr_info *ycbcr_info =\n      vk_format_get_ycbcr_info(pCreateInfo->format);',
     `   struct vk_image native_layout = image->vk;
   if (kk_writable_msaa_requested(&image->vk)) {
      native_layout.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
      native_layout.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
      native_layout.create_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   }
   const struct vk_format_ycbcr_info *ycbcr_info =
      vk_format_get_ycbcr_info(pCreateInfo->format);`);
  kk('kk_image.c', '      kk_image_layout_init(dev, &image->vk,',
     '      kk_image_layout_init(dev, &native_layout,');
  kk('kk_image.c', `   return VK_SUCCESS;
}

static void
kk_image_plane_size_align_B`,
     `   VkResult result = kk_writable_msaa_image_init(dev, image,
                                                       pCreateInfo->initialLayout);
   if (result != VK_SUCCESS)
      vk_image_finish(&image->vk);
   return result;
}

static void
kk_image_plane_size_align_B`);
  kk('kk_image.c', `   *size_B_out = plane->layout.size_B;
   *align_B_out = plane->layout.align_B;`,
     `   *size_B_out = image->writable_msaa ? image->writable_msaa->total_size_B
                                          : plane->layout.size_B;
   *align_B_out = image->writable_msaa ? image->writable_msaa->align_B
                                           : plane->layout.align_B;`);
  kk('kk_image.c', `kk_image_finish(struct kk_device *dev, struct kk_image *image,
                const VkAllocationCallbacks *pAllocator)
{`,
     `kk_image_finish(struct kk_device *dev, struct kk_image *image,
                const VkAllocationCallbacks *pAllocator)
{
   kk_writable_msaa_image_finish(dev, image);`);
  kk('kk_image.c', `   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_BindImageMemory2`,
     `   return kk_writable_msaa_image_bind(dev, image);
}

VKAPI_ATTR VkResult VKAPI_CALL
kk_BindImageMemory2`);
  edit('src/kosmickrisp/vulkan/kk_image.c', s => s + '\n#include "kk-writable-msaa-image.h"\n');
  kk('kk_image_view.c', '#include "kk_image_view.h"',
     '#include "kk_image_view.h"\n#include "kk-writable-msaa.h"');
  kk('kk_image_view.c', `   return VK_SUCCESS;
}

void
kk_image_view_finish`,
     `   VkResult result = kk_writable_msaa_view_init(dev, view);
   if (result != VK_SUCCESS)
      kk_image_view_finish(dev, view);
   return result;
}

void
kk_image_view_finish`);
  kk('kk_image_view.c', `kk_image_view_finish(struct kk_device *dev, struct kk_image_view *view)
{`,
     `kk_image_view_finish(struct kk_device *dev, struct kk_image_view *view)
{
   kk_writable_msaa_view_finish(dev, view);`);
  edit('src/kosmickrisp/vulkan/kk_image_view.c', s => s + '\n#include "kk-writable-msaa-view.h"\n');

  kk('kk_descriptor_set.c', '#include "kk_image_view.h"',
     '#include "kk_image_view.h"\n#include "kk-writable-msaa.h"');
  kk('kk_descriptor_set.c', `      kk_sliced_3d_descriptor(&desc, &view->vk);
      kk_sparse_storage_descriptor(&desc, view);
   }

   assert(sizeof(desc) <= dst_size);`,
     `      kk_sliced_3d_descriptor(&desc, &view->vk);
      kk_sparse_storage_descriptor(&desc, view);
      kk_writable_msaa_storage_descriptor(&desc, view);
   }

   assert(sizeof(desc) <= dst_size);`);
  kk('kk_descriptor_set.c', `         desc[plane].image_min_lod_uint16 = min_lod;
      }`,
     `         desc[plane].image_min_lod_uint16 = min_lod;
         kk_writable_msaa_sampled_descriptor(&desc[plane], view,
                                             is_input_attachment);
      }`);
  kk('kk_nir_lower_descriptors.c', '   bool clamp_desc_array_bounds;',
     '   bool robust_msaa_defaults;\n   bool clamp_desc_array_bounds;');
  kk('kk_nir_lower_descriptors.c',
     '      .ssbo_addr_format = kk_buffer_addr_format(rs->storage_buffers),',
     `      .robust_msaa_defaults = rs->images ==
         VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2,
      .ssbo_addr_format = kk_buffer_addr_format(rs->storage_buffers),`);
  kk('kk_nir_lower_descriptors.c', 'static bool\nlower_image_intrin(',
     '#include "kk-writable-msaa-nir.h"\n\nstatic bool\nlower_image_intrin(');
  kk('kk_nir_lower_descriptors.c',
     `   nir_alu_type type;
   if (nir_intrinsic_has_atomic_op(intr)) {`,
     `   if (kk_lower_msaa_storage(b, intr, ctx))
      return true;

   nir_alu_type type;
   if (nir_intrinsic_has_atomic_op(intr)) {`);
  kk('kk_nir_lower_descriptors.c',
     `      nir_def *handle = NULL;
      if (tex->is_shadow) {`,
     `      if (kk_lower_msaa_fetch(b, tex, texture, ctx))
         return true;

      nir_def *handle = NULL;
      if (tex->is_shadow) {`);

  kk('kk_cmd_buffer.h', '   struct util_dynarray msrtss_images;',
     `   bool writable_msaa_meta;
   uint32_t writable_msaa_render_attachments;
   uint32_t writable_msaa_render_writes;
   struct util_dynarray msrtss_images;`);
  kk('kk_cmd_buffer.h', '   bool force_attachment_store;',
     '   bool force_attachment_store;\n   bool writable_msaa_loaded;');
  kk('kk_cmd_buffer.c', '#include "kk_cmd_buffer.h"',
     '#include "kk_cmd_buffer.h"\n#include "kk-writable-msaa.h"');
  kk('kk_cmd_buffer.c', `   cs_end(cmd);

   cmd->metal.render = mtl_new_render_command_encoder_with_descriptor(`,
     `   cs_end(cmd);
   kk_writable_msaa_prepare_render(cmd);

   cmd->metal.render = mtl_new_render_command_encoder_with_descriptor(`);
  kk('kk_cmd_buffer.c', '      flush_post_render_writes(cmd);',
     '      flush_post_render_writes(cmd);\n      kk_writable_msaa_publish_render(cmd);');
  kk('kk_cmd_buffer.c', `   bool renderpass_resolve = kk_attachment_do_renderpass_resolve(
      attachment, rendering_to_whole_framebuffer, aspect);`,
     `   if (attachment->iview && aspect == VK_IMAGE_ASPECT_COLOR_BIT)
      force_store |= container_of(attachment->iview->vk.image,
                                   struct kk_image, vk)->writable_msaa != NULL;
   bool renderpass_resolve = kk_attachment_do_renderpass_resolve(
      attachment, rendering_to_whole_framebuffer, aspect);`);
  kk('kk_shader.c', '      shader->info.fs.flat_outputs = fs_flat_output_mask(nir);',
     '      shader->info.outputs_written = nir->info.outputs_written;\n      shader->info.fs.flat_outputs = fs_flat_output_mask(nir);');
  kk('kk_cmd_draw.c', '#include "kk_cmd_buffer.h"',
     '#include "kk_cmd_buffer.h"\n#include "kk-writable-msaa.h"');
  kk('kk_cmd_draw.c', `kk_dispatch_draw(struct kk_cmd_buffer *cmd, struct kk_draw_data data)
{
   mtl_render_encoder *enc = cs_get_render(cmd);`,
     `kk_dispatch_draw(struct kk_cmd_buffer *cmd, struct kk_draw_data data)
{
   mtl_render_encoder *enc = cs_get_render(cmd);
   kk_writable_msaa_record_draw(cmd);`);
  kk('kk_cmd_meta.c', '#include "kk_image_view.h"',
     '#include "kk_image_view.h"\n#include "kk-writable-msaa.h"');
  edit('src/kosmickrisp/vulkan/kk_cmd_meta.c', s => s + '\n#include "kk-writable-msaa-meta.h"\n');

  kk('kk_cmd_copy.c', '#include "kk_cmd_buffer.h"',
     '#include "kk_cmd_buffer.h"\n#include "kk-writable-msaa.h"');
  kk('kk_cmd_copy.c', '   size_t buffer_size = 0u;',
     '   if (kk_writable_msaa_copy_image(cmd, pCopyImageInfo))\n      return;\n\n   size_t buffer_size = 0u;');
  change('src/vulkan/runtime/vk_render_pass.h',
     'uint32_t\nvk_command_buffer_get_attachment_layout(',
     `const struct vk_image_view *
vk_command_buffer_get_attachment_view(const struct vk_command_buffer *cmd_buffer,
                                       uint32_t attachment);

uint32_t
vk_command_buffer_get_attachment_layout(`);
  change('src/vulkan/runtime/vk_render_pass.c',
     'uint32_t\nvk_command_buffer_get_attachment_layout(',
     `const struct vk_image_view *
vk_command_buffer_get_attachment_view(const struct vk_command_buffer *cmd_buffer,
                                       uint32_t attachment)
{
   assert(cmd_buffer->render_pass);
   assert(attachment < cmd_buffer->render_pass->attachment_count);
   return cmd_buffer->attachments[attachment].image_view;
}

uint32_t
vk_command_buffer_get_attachment_layout(`);
}
