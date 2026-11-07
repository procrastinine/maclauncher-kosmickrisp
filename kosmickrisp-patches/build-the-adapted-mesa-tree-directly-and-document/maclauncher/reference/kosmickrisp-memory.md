# KosmicKrisp memory

MacLauncher KosmicKrisp exposes two Vulkan memory types on its device-local memory heap. Ordinary device-local allocations without host visibility use private Metal placement heaps. Images allocated from these heaps retain the existing GPU-optimized texture layout, allowing Metal to use its texture compression. Buffers, images and suballocations use the same heap binding path.

Host-visible memory remains shared, host-coherent and host-cached. Uploads, readbacks, host image copies and imported host pointers use this type. Private memory has no CPU mapping; transfers to or from host-visible resources provide CPU access. Internal command, descriptor and shader uploads continue to use shared storage.

Imported Metal heaps must match the selected memory type's storage mode. The Metal handle query reports only compatible types, and host-pointer queries report only the shared type.

No lazily allocated memory type is exposed. Transient attachments use ordinary device-local memory and retain their contents across Metal encoder boundaries according to Vulkan load and store operations.

`VK_EXT_memory_priority` accepts allocation-time and dynamic priority hints. Priorities above 0.5 request additional idle residency through a shared Metal residency set. Default and lower priorities retain the common execution-residency protection. The priority has two effective classes; it does not specify physical page placement or eviction order. Imported aliases retain one native residency registration until the final alias is freed.

With `VK_EXT_pageable_device_local_memory` enabled, the common residency set protects allocations during queued execution without requesting whole-device idle retention. High-priority allocations retain their additional idle residency request. Contents and device addresses remain valid across priority changes; no discardable or purgeable storage is used. Applications enabling neither feature retain the common whole-device residency policy.

`VK_EXT_image_compression_control` supports default lossless compression policy and disabled compression. Disabled images set Metal GPU-optimized contents to false. Image-format and subresource-layout queries report the selected policy and no fixed-rate compression. Unsupported fixed-rate requests retain the default policy.

`VK_MESA_image_alignment_control` exposes maximum-alignment hints of 4, 16 and 64 KiB. Image memory requirements use Metal's minimum viable layout, including compression metadata. The native minimum remains authoritative when a requested maximum cannot be met; alignment hints do not force decompression or add allocation padding.

[External OPAQUE_FD allocations](KOSMICKRISP_EXTERNAL_MEMORY.md) use shared kernel backing with independent descriptor ownership. CPU mapping follows the selected Vulkan memory type.

Non-imported Vulkan allocations of at least 64 KiB backed by ordinary placement heaps have a 64 KiB-aligned logical GPU base. Native heap padding is excluded from the logical allocation size. Buffers, optimal images and 3D array aliases translate the same logical memory offset to the same physical heap origin. Buffer-relative and linear-image offsets remain relative to their native buffer. Suballocations retain their Vulkan binding-offset alignment requirements.

Retained Metal heap exports preserve the logical region when reimported by KosmicKrisp, including after the original Vulkan memory object is freed. First foreign Metal heaps and externally backed no-copy allocations, including host-pointer imports and FD-backed buffers, retain their existing payload origin and native alignment. `VK_VALVE_buffer_device_address_allocation_alignment` is not exposed.
