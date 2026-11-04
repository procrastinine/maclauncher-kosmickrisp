# Zero-initialized device memory

MacLauncher KosmicKrisp supplies `VK_EXT_zero_initialize_device_memory` through
`zeroInitializeDeviceMemory` and `VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT`.
For allocations requesting this flag, the driver clears the entire backing
buffer before returning the allocation. Host-visible shared memory is cleared on
the CPU. Device-local private memory is filled with zeros by a Metal blit; the
driver waits for that fill to complete before the memory can be used by any queue.
Unflagged allocations use the ordinary allocation path without a clear submission
or completion wait. [Memory types](kosmickrisp-memory.md) describes the storage types.

Imported heap and host allocations preserve their supplied contents. Vulkan
forbids the zero-initialize allocation flag on memory imports.

Images created with `VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT` use Metal storage
without GPU content optimization. This preserves cleared zero texel bits across
mip levels, array layers and image-layout transitions. Apple lossless texture
compression is disabled for these images. The initial image layout also accepts
backing explicitly zeroed by the application.

Other initial image layouts use the normal Metal storage selection. Shader
compilation, graphics pipelines and draw commands use the shared driver paths.

The adaptations are the `zero-initialize-device-memory` capability and its
`zero-initialize-allocation-clear` correction in the
[source catalog](../../src/kosmickrisp/).
