// SPDX-License-Identifier: MIT
export function apply({edit}) {
// Direct Wine loaders probe these exports instead of the ICD loader ABI.
// Keep the normal ICD lookup strict; tolerate promoted core names only on
// the direct entry point used by older Wine instances.
edit('src/kosmickrisp/vulkan/kk_instance.c', s => s + `
#if DETECT_OS_APPLE
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance handle, const char *name)
{
   PFN_vkVoidFunction func = kk_GetInstanceProcAddr(handle, name);
   if (func || !handle || !name)
      return func;
   VK_FROM_HANDLE(kk_instance, instance, handle);
   func = vk_instance_dispatch_table_get_if_supported(
      &instance->vk.dispatch_table, name, VK_API_VERSION_1_4,
      &instance->vk.enabled_extensions);
   if (func) return func;
   func = vk_physical_device_dispatch_table_get_if_supported(
      &kk_physical_device_trampolines, name, VK_API_VERSION_1_4,
      &instance->vk.enabled_extensions);
   if (func) return func;
   return vk_device_dispatch_table_get_if_supported(
      &kk_device_trampolines, name, VK_API_VERSION_1_4,
      &instance->vk.enabled_extensions, NULL);
}

#ifdef VK_USE_PLATFORM_METAL_EXT
PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vkCreateMetalSurfaceEXT(VkInstance instance,
   const VkMetalSurfaceCreateInfoEXT *info,
   const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
   PFN_vkCreateMetalSurfaceEXT create = (PFN_vkCreateMetalSurfaceEXT)
      kk_GetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");
   return create ? create(instance, info, allocator, surface)
                 : VK_ERROR_EXTENSION_NOT_PRESENT;
}
#endif
#endif
`);
edit('src/kosmickrisp/vulkan/kk_device.c', s => s + `
#if DETECT_OS_APPLE
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name)
{
   return kk_GetDeviceProcAddr(device, name);
}
#endif
`);
}
