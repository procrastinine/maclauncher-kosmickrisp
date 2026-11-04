# KosmicKrisp device address commands

VK_KHR_device_address_commands records transfers, indirect operations and vertex, index and transform-feedback bindings with GPU address ranges. Buffer device addresses refer to the same storage as their Vulkan buffers, including aliased bindings.

Copy, fill and update commands share Mesa's address-based meta implementation. Image transfers retain the existing Vulkan format conversion and Metal image-copy paths. Address-based image transfers stage copied texel rows in bounded command-buffer allocations; row and image padding remain untouched. Supported image formats and tilings follow the ordinary image-copy capabilities.

Address-range barriers use the same encoder synchronization and queue-family ownership rules as buffer barriers. Binary and timeline semaphores order transfers across the [queue families](KOSMICKRISP_QUEUES.md), including the dedicated transfer family.

The [device-address-commands module](../../src/kosmickrisp/) supplies the capability. Protected and sparse memory requirements remain governed by the corresponding device features.
