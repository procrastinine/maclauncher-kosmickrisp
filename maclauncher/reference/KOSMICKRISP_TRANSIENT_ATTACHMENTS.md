# Render encoding and transient attachments

KosmicKrisp retains a Metal render encoder across framebuffer-local dependencies with `VK_DEPENDENCY_BY_REGION_BIT`. Attachment color and depth/stencil accesses stay ordered within the rendering scope. Texture reads, storage dependencies and depth/stencil input reads retain the encoder boundaries required for visibility.

Adjacent dynamic-rendering scopes with `VK_RENDERING_SUSPENDING_BIT` and `VK_RENDERING_RESUMING_BIT` share their encoder when their attachments, render area, layer count, view mask and rasterization sample count match. This includes scopes recorded in separate command buffers and submitted in order. Graphics bindings and push constants retain Vulkan command-buffer state boundaries. A genuine compute, transfer or meta boundary preserves attachment contents through the ordinary store/load path.

Primary recording uses Mesa's arena-backed command queue. Ordinary primary buffers without descriptor heaps encode at `vkEndCommandBuffer`. Heap-enabled buffers and buffers containing suspended or resumed scopes use the shared submission replay path so descriptor writes and continuations are consumed in submission order. Drawable waits and signals surround each native commit once. Resubmission replays the same command representation. Internal encoding buffers remain alive until GPU completion.

## Lazy memory

The device exposes a `DEVICE_LOCAL | LAZILY_ALLOCATED` memory type for transient images. Buffers and images without `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` exclude this type from their memory requirements. External-memory images retain the private/shared import and export paths. Lazy memory is not host visible.

Eligible transient attachments use `MTLStorageModeMemoryless`. Their contents remain on tile across local dependencies and rendering continuations. Native color resolves write the single-sampled resolve attachment without storing the multisampled source. Lazy allocations start with no persistent backing; `vkGetDeviceMemoryCommitment` reports zero while no private backing is required.

Storage selection precedes the first encoder. Memoryless storage applies to a single bound, single-mip two-dimensional attachment with no input-attachment usage or image-creation flags, a complete render area, discarded final contents, and a command sequence that uses fixed native rasterization without an encoder restart. Direct triangle-list, triangle-strip and point draws with fixed fill rasterization use this path. Dependencies that include fragment-shader execution, custom sample locations, indirect draws, secondary execution, queries, dynamic pass changes and Poly or meta work retain private backing. A pass containing a multisampled storage attachment also retains private backing for its transient attachments before any sample-preserving conversion can split the encoder.

Loads, stores, partial render areas, input attachments, aliasing and shader resolves commit the allocation through the existing private placement-heap allocator. This preserves contents across required encoder boundaries. A zero-initialize allocation flag applies through the shared initialization path when backing is committed. Commitment is reported for the logical allocation size once private backing is allocated. Private textures and their views remain owned by the original image and view objects. Storage fallback does not change the Vulkan attachment operations or memory-type compatibility.

The lazy-memory and attachment contracts follow [Vulkan memory allocation](https://docs.vulkan.org/spec/latest/chapters/memory.html) and [dynamic rendering](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingInfo.html).
