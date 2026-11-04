/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "kk_wsi.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_dispatch_trampolines.h"
#include "kk_entrypoints.h"
#include "kk_image.h"
#include "kk_instance.h"
#include "wsi_common.h"

#include "kosmickrisp/bridge/mtl_bridge.h"

static PFN_vkVoidFunction
kk_instance_get_proc_addr_unchecked(const struct vk_instance *instance,
                                    const char *name)
{
   PFN_vkVoidFunction func;

   if (instance == NULL || name == NULL)
      return NULL;

   func = vk_instance_dispatch_table_get(&instance->dispatch_table, name);
   if (func != NULL)
      return func;

   func = vk_physical_device_dispatch_table_get(&kk_physical_device_trampolines,
                                                name);
   if (func != NULL)
      return func;

   func = vk_device_dispatch_table_get(&kk_device_trampolines, name);
   if (func != NULL)
      return func;

   return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
kk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(kk_physical_device, pdev, physicalDevice);
   return kk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
}

static VkResult
kk_bind_drawable_to_vkimage(VkImage vk_image, void *drawable)
{
   VK_FROM_HANDLE(kk_image, image, vk_image);
   mtl_texture *texture = mtl_drawable_get_texture(drawable);

   /* This should only be called for swapchain binding. */
   assert(image->plane_count == 1);
   struct kk_image_plane *plane = &image->planes[0];
   if (plane->mtl_handle)
      mtl_release(plane->mtl_handle);
   if (plane->mtl_handle_array)
      mtl_release(plane->mtl_handle_array);
   plane->mtl_handle = mtl_retain(texture);
   plane->mtl_handle_array = NULL;
   plane->addr = mtl_texture_get_gpu_resource_id(texture);

   return VK_SUCCESS;
}

static void
kk_wsi_cmd_blit_image(VkCommandBuffer vk_cmd, const VkBlitImageInfo2 *blit_info,
                      bool opaque_alpha)
{
   VK_FROM_HANDLE(kk_image, src, blit_info->srcImage);

   /* The blit samples the source through a view the meta path creates for
    * itself, and that view is where the alpha gets dropped. The value is a
    * property of the swapchain, so it never changes for a given image. */
   src->wsi_opaque_alpha = opaque_alpha;

   VK_FROM_HANDLE(kk_image, dst, blit_info->dstImage);
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
   kk_CmdBlitImage2(vk_cmd, blit_info);
}

static void
kk_encode_drawable_present(VkCommandBuffer vk_cmd, void *drawable)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, vk_cmd);
   mtl_retain(drawable);
   cmd->drawable = drawable;
}

static struct vk_queue *kk_get_blit_queue(VkDevice device);

static void *
kk_get_mtl4_command_queue(VkDevice device)
{
   struct vk_queue *queue = kk_get_blit_queue(device);
   return queue ? container_of(queue, struct kk_queue, vk)->mtl_handle : NULL;
}

static struct vk_queue *
kk_get_blit_queue(VkDevice device)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   if (!dev->has_queue)
      return NULL;

   simple_mtx_lock(&dev->blit_queue_mutex);
   if (!dev->has_blit_queue) {
      const float priority = 1.0f;
      const VkDeviceQueueCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &priority,
      };
      VkResult result = kk_queue_init(dev, &dev->blit_queue, &info, 0);
      if (result == VK_SUCCESS)
         dev->has_blit_queue = true;
      else
         mesa_logw("kk: failed to create the WSI queue");
   }
   struct vk_queue *queue = dev->has_blit_queue ? &dev->blit_queue.vk : &dev->queue.vk;
   simple_mtx_unlock(&dev->blit_queue_mutex);
   return queue;
}

VkResult
kk_init_wsi(struct kk_physical_device *pdev)
{
   struct wsi_device_options wsi_options = {.sw_device = false};
   struct wsi_device *wsi = &pdev->wsi_device;
   VkResult result =
      wsi_device_init(wsi, kk_physical_device_to_handle(pdev), kk_wsi_proc_addr,
                      &pdev->vk.instance->alloc,
                      0u,   // Not relevant for metal wsi
                      NULL, // Not relevant for metal
                      &wsi_options);
   if (result != VK_SUCCESS)
      return result;

   wsi->metal.bind_drawable_to_vkimage = kk_bind_drawable_to_vkimage;
   wsi->metal.encode_drawable_present = kk_encode_drawable_present;
   wsi->metal.cmd_blit_image = kk_wsi_cmd_blit_image;
   wsi->metal.get_mtl4_command_queue = kk_get_mtl4_command_queue;
   wsi->get_blit_queue = kk_get_blit_queue;

   pdev->vk.wsi_device = wsi;

   return result;
}

void
kk_finish_wsi(struct kk_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}
