# Extended image flags

MacLauncher KosmicKrisp supports `VK_KHR_extended_flags` and its `extendedFlags` feature. Image creation and image format queries accept the 64-bit create and usage structures. Their values replace the corresponding legacy flags. Image views accept `VkImageViewUsage2CreateInfoKHR`; separate stencil usage uses `VkImageStencilUsage2CreateInfoKHR`.

The extension preserves the format, sample-count, allocation and usage limits of the underlying image implementation. It adds no image usage or create flags beyond the supported Vulkan features.

`VkFormatProperties4KHR` reports an additional format-feature namespace. Its fields are zero because the selected Vulkan registry defines no flags in that namespace. Existing format capabilities remain in `VkFormatProperties3` and the legacy format properties, including when both structures share a query chain.
