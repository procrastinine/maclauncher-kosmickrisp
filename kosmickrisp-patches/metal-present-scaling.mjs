// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

// MoltenVK 5189f8de86ec365860f63bf1da736815cfaea5cd, MVKSwapchain.mm,
// informed deliberate-scaling status; rectangles use Mesa's existing GPU blit.
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-wsi.h'];
export const id = 'metal-present-scaling';
export function apply({base: sourceRoot, edits}) {
  const edit = (relative, apply) => {
    let source = edits.get(relative) ?? fs.readFileSync(path.join(sourceRoot, relative), 'utf8');
    const replace = (from, to) => {
      if (!source.includes(from)) throw Error(`${relative}: missing WSI anchor ${from.slice(0, 80)}`);
      source = source.replace(from, to);
    };
    apply(replace);
    edits.set(relative, source);
  };
  const kk = (file, apply) => edit('src/kosmickrisp/vulkan/' + file, apply);
  kk('kk_wsi.c', r => {
    r('   kk_CmdBlitImage2(vk_cmd, blit_info);', `   VK_FROM_HANDLE(kk_image, dst, blit_info->dstImage);
   const VkImageBlit2 *region = blit_info->pRegions;
   if (region->dstOffsets[0].x || region->dstOffsets[0].y ||
       region->dstOffsets[1].x != dst->vk.extent.width ||
       region->dstOffsets[1].y != dst->vk.extent.height) {
      const VkClearColorValue black = {.float32 = {0, 0, 0, opaque_alpha ? 1 : 0}};
      const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      kk_CmdClearColorImage(vk_cmd, blit_info->dstImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
      const VkImageMemoryBarrier2 barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = blit_info->dstImage,
         .subresourceRange = range,
      };
      const VkDependencyInfo dependency = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier,
      };
      kk_CmdPipelineBarrier2(vk_cmd, &dependency);
   }
   kk_CmdBlitImage2(vk_cmd, blit_info);`);
  });
  const wsi = (file, apply) => edit('src/vulkan/wsi/' + file, apply);
  edits.set('src/vulkan/wsi/wsi_metal_scaling.h', fs.readFileSync(new URL('./kosmickrisp-wsi.h', import.meta.url), 'utf8'));
  wsi('wsi_common_metal_layer.h', r => r('void\nwsi_metal_layer_set_immediate', `void
wsi_metal_layer_set_drawable_size(const CAMetalLayer *metal_layer,
                                 uint32_t width, uint32_t height);

void
wsi_metal_layer_set_immediate`));
  wsi('wsi_common_metal_layer.m', r => r('void\nwsi_metal_layer_set_immediate', `void
wsi_metal_layer_set_drawable_size(const CAMetalLayer *metal_layer,
                                 uint32_t width, uint32_t height)
{
   CGSize size = CGSizeMake(width, height);
   if (!CGSizeEqualToSize(metal_layer.drawableSize, size))
      metal_layer.drawableSize = size;
}

void
wsi_metal_layer_set_immediate`));
  wsi('wsi_common_metal.c', r => {
    r('#include "wsi_common_metal_layer.h"', '#include "wsi_common_metal_layer.h"\n#include "wsi_metal_scaling.h"');
    r('      wsi->BeginCommandBuffer(image->blit.cmd_buffers[i], &begin_info);', `      result = wsi->BeginCommandBuffer(image->blit.cmd_buffers[i], &begin_info);
      if (result != VK_SUCCESS)
         return result;`);
    r('      result = wsi->EndCommandBuffer(image->blit.cmd_buffers[i]);', `      result = wsi->EndCommandBuffer(image->blit.cmd_buffers[i]);
      if (result != VK_SUCCESS)
         return result;`);
    r('         /* TODO: support scaling */\n', '');
    r(`         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;`, `         scaling->supportedPresentScaling = wsi_device->sw ? 0 :
            VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR |
            VK_PRESENT_SCALING_ASPECT_RATIO_STRETCH_BIT_KHR |
            VK_PRESENT_SCALING_STRETCH_BIT_KHR;
         scaling->supportedPresentGravityX = wsi_device->sw ? 0 :
            VK_PRESENT_GRAVITY_MIN_BIT_KHR | VK_PRESENT_GRAVITY_MAX_BIT_KHR |
            VK_PRESENT_GRAVITY_CENTERED_BIT_KHR;
         scaling->supportedPresentGravityY = scaling->supportedPresentGravityX;`);
    r('   CAMetalDrawable *drawable;\n', '   CAMetalDrawable *drawable;\n   VkExtent2D drawable_extent;\n');
    r('   VkExtent2D extent;\n', `   VkExtent2D extent;
   VkPresentScalingFlagsKHR scaling;
   VkPresentGravityFlagsKHR gravity_x, gravity_y;
`);
    r('static VkResult\nwsi_cmd_blit_image_to_image', `static VkResult
wsi_metal_resize_blit_image(struct wsi_metal_swapchain *chain,
                            struct wsi_metal_image *image, VkExtent2D extent)
{
   if (image->base.blit.image && image->drawable_extent.width == extent.width &&
       image->drawable_extent.height == extent.height)
      return VK_SUCCESS;
   VkImageCreateInfo create = chain->base.image_info.create;
   create.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   /* This drawable wrapper inherits view formats, without source-memory metadata. */
   VkImageUsageFlags2CreateInfoKHR usage = chain->base.image_info.usage2;
   usage.usage |= VK_IMAGE_USAGE_2_COLOR_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_2_TRANSFER_DST_BIT_KHR;
   usage.pNext = NULL;
   create.pNext = &usage;
   VkImageFormatListCreateInfo formats = chain->base.image_info.format_list;
   if (formats.viewFormatCount) {
      formats.pNext = create.pNext;
      create.pNext = &formats;
   }
   create.extent = (VkExtent3D){extent.width, extent.height, 1};
   VkImage target;
   VkResult result = chain->base.wsi->CreateImage(chain->base.device, &create,
                                                &chain->base.alloc, &target);
   if (result != VK_SUCCESS)
      return result;
   if (image->base.blit.image)
      chain->base.wsi->DestroyImage(chain->base.device, image->base.blit.image,
                                    &chain->base.alloc);
   image->base.blit.image = target;
   image->drawable_extent = extent;
   return VK_SUCCESS;
}

static VkResult
wsi_cmd_blit_image_to_image`);
    r(`      const VkImageSubresourceLayers subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .baseArrayLayer = 0,
         .layerCount = 1,
      };

      const VkOffset3D extent = {
         .x = info->create.extent.width,
         .y = info->create.extent.height,
         .z = 1,
      };
      const VkImageBlit2 image_blit = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
         .srcSubresource = subresource,
         .srcOffsets = { { .x = 0, .y = 0, .z = 0 }, extent },
         .dstSubresource = subresource,
         .dstOffsets = { { .x = 0, .y = 0, .z = 0 }, extent },
      };`, `      const VkImageBlit2 image_blit = wsi_metal_present_region(
         metal_chain->extent, metal_image->drawable_extent, metal_chain->scaling,
         metal_chain->gravity_x, metal_chain->gravity_y);`);
    r('   while (1) {\n      /* Try to acquire', `   VkExtent2D target = chain->extent;
   if (chain->scaling) {
      wsi_metal_layer_size(chain->surface->pLayer, &target.width, &target.height);
      if (!target.width || !target.height)
         return info->timeout ? VK_TIMEOUT : VK_NOT_READY;
      wsi_metal_layer_set_drawable_size(chain->surface->pLayer, target.width, target.height);
   }
   uint32_t next = chain->current_image_index;
   while (chain->images[next].drawable) {
      next = (next + 1) % chain->base.image_count;
      if (next == chain->current_image_index)
         return info->timeout ? VK_TIMEOUT : VK_NOT_READY;
   }
   /* The previous present fence protects the recorded blit and its image. */
   if (chain->base.fences[next]) {
      VkResult result = chain->base.wsi->WaitForFences(chain->base.device, 1,
         &chain->base.fences[next], true, info->timeout);
      if (result != VK_SUCCESS)
         return result == VK_TIMEOUT && !info->timeout ? VK_NOT_READY : result;
   }
   while (1) {
      /* Try to acquire`);
    r('         uint32_t i = (chain->current_image_index++) % chain->base.image_count;', `         uint32_t i = next;
         chain->current_image_index = (i + 1) % chain->base.image_count;`);
    r('         *image_index = i;\n         image->drawable = drawable;', `         *image_index = i;
         if (!wsi_chain->wsi->sw) {
            VkResult result = wsi_metal_resize_blit_image(chain, image, target);
            if (result != VK_SUCCESS) {
               wsi_metal_release_drawable(drawable);
               return result;
            }
         }
         image->drawable = drawable;`);
    r('            wsi_cmd_blit_image_to_image(wsi_chain, &wsi_chain->image_info, &image->base);', `            VkResult result = wsi_cmd_blit_image_to_image(wsi_chain, &wsi_chain->image_info, &image->base);
            if (result != VK_SUCCESS) {
               wsi_metal_release_drawable(image->drawable);
               image->drawable = NULL;
               return result;
            }`);
    r('         return VK_NOT_READY;\n   }', '         return info->timeout ? VK_TIMEOUT : VK_NOT_READY;\n   }');
    r('   bool is_optimal = (width == chain->extent.width && height == chain->extent.height);', '   bool is_optimal = chain->scaling || (width == chain->extent.width && height == chain->extent.height);');
    r(`   /* We perform blit through rendering */
   VkImageCreateInfo blit_create = chain->image_info.create;
   blit_create.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   result = wsi->CreateImage(chain->device, &blit_create,
                             &chain->alloc, &image->blit.image);`, `   result = wsi_metal_resize_blit_image((struct wsi_metal_swapchain *)metal_chain,
                                       metal_image, metal_chain->extent);`);
    r('   VkResult result = wsi_metal_layer_configure(metal_surface->pLayer,\n      pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,', `   const VkSwapchainPresentScalingCreateInfoKHR *scaling =
      vk_find_struct_const(pCreateInfo->pNext, SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_KHR);
   const VkPresentScalingFlagsKHR behavior = !wsi_device->sw && scaling ? scaling->scalingBehavior : 0;
   VkExtent2D target = pCreateInfo->imageExtent;
   if (behavior) {
      wsi_metal_layer_size(metal_surface->pLayer, &target.width, &target.height);
      target.width = MAX2(target.width, 1);
      target.height = MAX2(target.height, 1);
   }
   VkResult result = wsi_metal_layer_configure(metal_surface->pLayer,
      target.width, target.height,`);
    r('   chain->extent = pCreateInfo->imageExtent;', `   chain->extent = pCreateInfo->imageExtent;
   chain->scaling = behavior;
   chain->gravity_x = scaling ? scaling->presentGravityX : 0;
   chain->gravity_y = scaling ? scaling->presentGravityY : 0;`);
  });
}
