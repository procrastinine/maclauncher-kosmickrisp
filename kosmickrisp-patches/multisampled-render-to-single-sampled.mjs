// SPDX-License-Identifier: MIT
// Implicit MSAA uses the shared attachment, blit and resolve implementations.
import fs from 'node:fs';
export const id = 'multisampled-render-to-single-sampled';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-msrtss.h'];
export function apply({edit, edits}) {
  const change = (file, from, to) => edit(file, (_s, replace) => replace(from, to));
  const kk = (file, from, to) => change('src/kosmickrisp/' + file, from, to);
  edits.set('src/kosmickrisp/vulkan/kk_msrtss.h',
    fs.readFileSync(new URL('./kosmickrisp-msrtss.h', import.meta.url), 'utf8'));
  kk('vulkan/kk_physical_device.c', '   .EXT_extended_dynamic_state = true,',
     '   .EXT_multisampled_render_to_single_sampled = true,\n   .EXT_extended_dynamic_state = true,');
  kk('vulkan/kk_physical_device.c', '      .dynamicRendering = true,',
     '      .multisampledRenderToSingleSampled = true,\n      .dynamicRendering = true,');
  kk('vulkan/kk_cmd_buffer.h', '   bool attachmentless;', '   bool attachmentless;\n   bool msrtss;');
  kk('vulkan/kk_cmd_draw.c', '   if (render->flags &\n       (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT))',
     '   if (!render->msrtss && (render->flags &\n       (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT)))');
  kk('vulkan/kk_cmd_buffer.h', 'struct kk_cmd_buffer {',
     'struct kk_msrtss_image {\n   VkImage image;\n   VkDeviceMemory memory;\n   struct kk_image_view *source;\n   VkImageView view;\n   VkSampleCountFlagBits samples;\n};\n\nstruct kk_cmd_buffer {');
  kk('vulkan/kk_cmd_buffer.h', '   struct util_dynarray ts_resolves;',
     '   struct util_dynarray msrtss_images;\n   struct util_dynarray ts_resolves;');
  kk('vulkan/kk_cmd_buffer.c', '   cmd->ts_resolves = UTIL_DYNARRAY_INIT;',
     '   cmd->msrtss_images = UTIL_DYNARRAY_INIT;\n   cmd->ts_resolves = UTIL_DYNARRAY_INIT;');
  kk('vulkan/kk_cmd_buffer.c', '   util_dynarray_fini(&cmd->ts_resolves);',
     '   util_dynarray_fini(&cmd->msrtss_images);\n   util_dynarray_fini(&cmd->ts_resolves);');
  kk('vulkan/kk_cmd_buffer.c', '   kk_cmd_release_dynamic_ds_state(cmd);',
     `   /* Internal views must go before their command-owned backing images. */
   vk_meta_object_list_reset(&dev->vk, &cmd->vk.meta_objects);
   util_dynarray_foreach(&cmd->msrtss_images, struct kk_msrtss_image, image) {
      kk_DestroyImage(kk_device_to_handle(dev), image->image, NULL);
      kk_FreeMemory(kk_device_to_handle(dev), image->memory, NULL);
   }
   util_dynarray_clear(&cmd->msrtss_images);
   kk_cmd_release_dynamic_ds_state(cmd);`);
  kk('vulkan/kk_cmd_draw.c', 'VKAPI_ATTR void VKAPI_CALL\nkk_CmdBeginRendering(',
     'static void\nkk_begin_rendering(');
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s + '\n#include "kk_msrtss.h"\n');
  kk('vulkan/kk_cmd_buffer.h', 'void kk_meta_resolve_rendering(',
     'void kk_meta_expand_msrtss(struct kk_cmd_buffer *cmd, struct kk_image_view *src,\n                           VkImage dst, VkImageAspectFlags aspects);\n\nvoid kk_meta_resolve_rendering(');
  edit('src/kosmickrisp/vulkan/kk_cmd_meta.c', s => s + `
void
kk_meta_expand_msrtss(struct kk_cmd_buffer *cmd, struct kk_image_view *src,
                     VkImage dst, VkImageAspectFlags aspects)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   VK_FROM_HANDLE(kk_image, dst_image, dst);
   const VkExtent3D extent = vk_image_mip_level_extent(src->vk.image,
                                                     src->vk.base_mip_level);
   const VkImageBlit2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
      .srcSubresource = {aspects, src->vk.base_mip_level,
                         src->vk.base_array_layer, src->vk.layer_count},
      .srcOffsets = {{0, 0, 0}, {extent.width, extent.height, 1}},
      .dstSubresource = {aspects, 0, 0, src->vk.layer_count},
      .dstOffsets = {{0, 0, 0}, {extent.width, extent.height, 1}},
   };
   struct kk_meta_save save;
   kk_meta_begin(cmd, &save, VK_PIPELINE_BIND_POINT_GRAPHICS, "meta:MSRTSS load");
   vk_meta_blit_image(&cmd->vk, &dev->meta, src->vk.image,
      src->vk.view_format, VK_IMAGE_LAYOUT_GENERAL, &dst_image->vk,
      src->vk.view_format, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
   kk_meta_end(cmd, &save, VK_PIPELINE_BIND_POINT_GRAPHICS);
}
`);
  kk('vulkan/kk_cmd_meta.c', '#include "kk_physical_device.h"',
     '#include "kk_physical_device.h"\n#include "kk_image.h"\n#include "kk_image_view.h"');
  kk('vulkan/kk_format.c', '      switch (sType) {', `      switch (sType) {
      case VK_STRUCTURE_TYPE_SUBPASS_RESOLVE_PERFORMANCE_QUERY_EXT: {
         /* Private MSAA backing and LOAD replication are not a free resolve. */
         ((VkSubpassResolvePerformanceQueryEXT *)ext)->optimal = VK_FALSE;
         break;
      }`);
  // A destination sample count is part of the same meta pipeline key. Ordinary
  // blits and resolves retain sample count one, without a second shader path.
  change('src/vulkan/runtime/vk_meta_blit_resolve.c',
     '   VkSampleCountFlagBits src_samples;',
     '   VkSampleCountFlagBits src_samples;\n   VkSampleCountFlagBits dst_samples;');
  change('src/vulkan/runtime/vk_meta_blit_resolve.c', '      .samples = 1,',
     '      .samples = key->dst_samples ? key->dst_samples : VK_SAMPLE_COUNT_1_BIT,');
  change('src/vulkan/runtime/vk_meta_blit_resolve.c',
     '   key.src_samples = src_image->samples;',
     '   key.src_samples = src_image->samples;\n   key.dst_samples = dst_image->samples;');
}
