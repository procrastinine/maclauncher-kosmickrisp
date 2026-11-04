# Fragment interlock

MacLauncher KosmicKrisp exposes `VK_EXT_fragment_shader_interlock` with `fragmentShaderPixelInterlock` and `fragmentShaderSampleInterlock` on supported Metal 4 Apple GPUs. Ordered and unordered regions synchronize overlapping fragment invocations at the same framebuffer pixel or sample, respectively.

Sample interlock and single-sample pixel interlock use an explicit Metal raster-order device buffer to bracket the region. The SPIR-V acquire and release fences order storage-buffer, storage-image and physical-buffer-address accesses inside it. Existing descriptor layouts and address lowering are shared with ordinary shaders. Discard releases an active region before helper writes are suppressed.

Multisampled pixel-interlock draws use a pixel-scope envelope because native raster-order groups do not provide exclusion across different samples of one pixel. Pre-rasterization outputs are captured once and reused for fragment execution and attachment replay. Pixel-frequency fragment bodies execute once per primitive and pixel; sample-frequency bodies iterate the captured samples. Attachment fetch, blending, sample masks, alpha-to-coverage and depth/stencil updates operate on the coherent per-sample shadow before replay.

The envelope uses the shared asynchronous queue continuation mechanism. Later work and application signals remain ordered behind its continuation. Captured resources remain alive through completion or cancellation; vertex and geometry side effects are not replayed.

Enabling interlock allocates one 128 KiB guard buffer per device and binds it when command buffers are created. Native interlock shaders access the guard at region boundaries. Sample interlock and single-sample pixel interlock retain the existing draw-recording and descriptor-root upload paths. Devices that do not enable interlock allocate no guard buffer.

`fragmentShaderShadingRateInterlock` is false. Native groups do not provide exclusion across a multi-pixel shading-rate region.

vkd3d-proton requires both pixel and sample interlock for rasterizer-ordered views. Its feature-level 12_1 gate also requires feature-level 12_0 capabilities and conservative rasterization. A feature-level query reports the consumer's capability selection; it does not establish application rendering or conformance.

Color attachment ordering is described in the [ordered attachment read reference](KOSMICKRISP_ATTACHMENT_ACCESS.md). On Apple GPU families 7–9, `rasterizationOrderDepthAttachmentAccess` and `rasterizationOrderStencilAttachmentAccess` remain false. Depth and stencil texture reads observe stored backing rather than updates from earlier overlapping primitives within the same active draw. Metal fragment inputs expose neither aspect, and fragment functions cannot take imageblock arguments. A store/load boundary makes the updates readable; it does not provide attachment ordering between primitives in one draw.

The scope definitions are in the [Vulkan fragment interlock specification](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-fragment-interlock). Native resource ordering and device fences are specified in the [Metal Shading Language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf), sections 5.2.1.2 and 6.16.3.
