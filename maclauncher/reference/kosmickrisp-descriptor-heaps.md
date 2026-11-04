# KosmicKrisp descriptor heaps

MacLauncher KosmicKrisp exposes `VK_EXT_descriptor_heap` on arm64 and x64. Resource and sampler heaps use buffer device addresses. Shader mappings support constant offsets, push indices, indirect indices and addresses, and inline push or resource-heap data through Mesa's shared mapping lowering. Descriptor sets and descriptor heaps share native resource and metadata handling and can be used in the same command buffer.

`vkWriteResourceDescriptorsEXT` writes sampled images, storage images, uniform and storage buffers, and uniform and storage texel buffers. `vkWriteSamplerDescriptorsEXT` writes native samplers and their addressing, filtering, LOD and border metadata. Embedded samplers use the same representation. Custom border colors use the extension's registration commands. Native image and texel views remain attached to their image or buffer; interned heap samplers remain attached to the device.

Devices with `descriptorHeap` enabled record Vulkan commands for native encoding at submission. Descriptor writes completed after command recording and before submission supply the current resource IDs to that encoding. Repeated submissions use the same command replay path. This records and replays CPU commands even when a heap-enabled device uses ordinary descriptor-set pipelines. Generated graphics and compute commands share snapshot root preparation; subsequent ordinary pipelines retain their descriptor-set bindings. Devices without the feature retain immediate primary-command encoding and the existing repeated-submission behavior. Drawable waits and signals surround the native command buffer that is actually submitted.

| Descriptor | Size | Alignment |
| --- | ---: | ---: |
| Sampler metadata pointer | 8 bytes | 8 bytes |
| Sampled image, including multi-plane images | 64 bytes | 8 bytes |
| Storage image | 16 bytes | 8 bytes |
| Texel buffer | Reported type size | 8 bytes |
| Uniform or storage buffer | 16 bytes | 8 bytes |

A sampler entry points to device-owned shared metadata. Sets and heaps use the same sampled-image writer and [64-byte auxiliary-word layout](kosmickrisp-descriptor-layout.md). For multi-planar images, `aux_resource_id` at byte 48 holds plane 1's native ID and `aux_word` at byte 56 holds plane 2's ID for three-plane images. Single-plane records retain both auxiliary words from their shared image-view writers. Multi-planar formats exclude cube-compatible and sparse image creation flags. Sampler metadata has no image identifier and uses the auxiliary words for its native sampler and clamp sampler IDs.

The image size property describes the largest image or texel descriptor. `vkGetPhysicalDeviceDescriptorSizeEXT` reports each descriptor type's size. Resource and sampler heap bases have 8-byte alignment. Storage-image and texel descriptors share an 8-byte native resource ID and two 32-bit metadata words. Texel record sizes and trailing metadata follow the shared type layout returned by the descriptor-size query. The same layout carries sliced-3D bounds, aligned texel prefixes and logical texel counts, including RGB32 uniform texel views.

Uniform and storage buffer records retain their 16-byte bounded-address layout. Sets, dynamic sets and heaps use one writer, which initializes the final 32-bit residency word to zero for ordinary or null buffers. Storage-buffer descriptor offsets have 4-byte alignment. Descriptor ranges retain their exact byte size, and robust accesses retain the 4-byte bounds granularity. Under-aligned 64-bit non-atomic loads and stores use 32-bit accesses without rounding the address or widening the range. Push data supports up to 4096 bytes. Ordinary push constants and heap push data share byte-range updates; unchanged bytes retain the existing root snapshot. Capture and replay, protected heaps and sparse heaps are unavailable.
