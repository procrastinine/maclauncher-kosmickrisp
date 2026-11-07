# Multisampled rendering to single-sampled attachments

MacLauncher KosmicKrisp provides `VK_EXT_multisampled_render_to_single_sampled` for multisample rendering into images whose stored representation has one sample per pixel. A render pass subpass or dynamic rendering scope selects its rasterization sample count through `VkMultisampledRenderToSingleSampledInfoEXT`. Attachments may mix that sample count with single-sampled images created with `VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT`.

For each single-sampled attachment, an internal multisample image holds the rendering samples. With `VK_ATTACHMENT_LOAD_OP_LOAD`, each stored pixel is copied to every sample; clear operations clear the multisample representation. An empty scope using `LOAD_OP_NONE` and `STORE_OP_NONE` preserves the original image without loading it into the internal image. At the end of the rendering scope, stored values are resolved into the original image. Floating-point and normalized color use average resolves, while integer color uses sample zero. Depth and stencil follow their selected resolve modes.

Full-frame color resolves use Metal render-pass resolves when the format supports them. Partial render areas, integer color and depth/stencil use Mesa's shared resolve shaders. The original attachment's mip level and array layers remain the destination. Internal images and memory belong to the command buffer and are released when it is reset or destroyed.

Private Metal storage preserves multisample values across encoder interruptions. Suspend and resume boundaries resolve and reload the single-sampled representation. Internal attachments use the existing device-local memory type; application memory types and format features retain their normal meaning. Resolve performance queries return `optimal = VK_FALSE` because this path allocates backing storage and may encode load or resolve draws.

The selected device and attachment format determine the supported sample counts. Legacy render passes and dynamic rendering share the same implementation through Mesa's render-pass conversion. Ordinary rendering without the extension uses the same attachment and resolve code.

The adaptation is registered as `multisampled-render-to-single-sampled` in the [capability catalog](../../src/kosmickrisp/).
