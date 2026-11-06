// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

// Mesa builds and initializes wsi_common_headless on macOS as well as Metal WSI.
export const id = 'headless-surface';
export function apply({base: sourceRoot, edits}) {
  const file = 'src/kosmickrisp/vulkan/kk_instance.c';
  const source = edits.get(file) ?? fs.readFileSync(path.join(sourceRoot, file), 'utf8');
  const anchor = '#ifndef VK_USE_PLATFORM_METAL_EXT\n   .EXT_headless_surface = true,\n#endif';
  if (!source.includes(anchor)) throw Error('Missing headless surface extension anchor');
  edits.set(file, source.replace(anchor, '   .EXT_headless_surface = true,'));
  const edit = (name, from, to) => {
    const file = 'src/vulkan/wsi/' + name;
    const text = edits.get(file) ?? fs.readFileSync(path.join(sourceRoot, file), 'utf8');
    if (!text.includes(from)) throw Error('Missing native headless image anchor: ' + name);
    edits.set(file, text.replace(from, to));
  };
  // A headless image needs ordinary optimal device memory. macOS has no DRM
  // backend, and using the CPU WSI path would introduce a pointless GPU copy.
  edit('wsi_common_private.h', '   WSI_IMAGE_TYPE_METAL,', '   WSI_IMAGE_TYPE_METAL,\n   WSI_IMAGE_TYPE_HEADLESS,');
  edit('wsi_common_private.h', 'struct wsi_base_image_params {', `struct wsi_image_info;

VkResult
wsi_headless_configure_image(const struct wsi_swapchain *chain,
                             const VkSwapchainCreateInfoKHR *create,
                             struct wsi_image_info *info);

struct wsi_base_image_params {`);
  edit('wsi_common.c', '   switch (params->image_type) {', `   switch (params->image_type) {
   case WSI_IMAGE_TYPE_HEADLESS:
      return WSI_SWAPCHAIN_NO_BLIT;`);
  edit('wsi_common.c', '   info->color_space = pCreateInfo->imageColorSpace;\n\n   switch (params->image_type) {', `   info->color_space = pCreateInfo->imageColorSpace;

   switch (params->image_type) {
#ifndef _WIN32
   case WSI_IMAGE_TYPE_HEADLESS:
      return wsi_headless_configure_image(chain, pCreateInfo, info);
#endif`);
  edit('wsi_common_headless.c', 'static VkResult\nwsi_headless_surface_create_swapchain', `static VkResult
wsi_headless_create_native_mem(const struct wsi_swapchain *chain,
                                const struct wsi_image_info *info,
                                struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkMemoryRequirements req;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &req);
   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
   };
   const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = req.size,
      .memoryTypeIndex = wsi_select_device_memory_type(wsi, req.memoryTypeBits),
   };
   return wsi->AllocateMemory(chain->device, &allocate, &chain->alloc, &image->memory);
}

VkResult
wsi_headless_configure_image(const struct wsi_swapchain *chain,
                             const VkSwapchainCreateInfoKHR *create,
                             struct wsi_image_info *info)
{
   VkResult result = wsi_configure_image(chain, create, 0, info);
   if (result != VK_SUCCESS)
      return result;
   info->image_type = WSI_IMAGE_TYPE_HEADLESS;
   info->create_mem = wsi_headless_create_native_mem;
   return VK_SUCCESS;
}

static VkResult
wsi_headless_surface_create_swapchain`);
  edit('wsi_common_headless.c', '   if (wsi_device->sw) {\n      cpu_params =', `#ifdef VK_USE_PLATFORM_METAL_EXT
   struct wsi_base_image_params native_params = {
      .image_type = WSI_IMAGE_TYPE_HEADLESS,
   };
   if (!wsi_device->sw && !wsi_device->supports_modifiers) {
      image_params = &native_params;
   } else
#endif
   if (wsi_device->sw) {
      cpu_params =`);
}
