# Ordered attachment reads

The KosmicKrisp adaptation exposes `VK_EXT_rasterization_order_attachment_access` with `rasterizationOrderColorAttachmentAccess` on Apple GPUs supported by the Metal 4 driver. Color input-attachment reads use Metal programmable blending to observe earlier color writes at the same framebuffer sample, in primitive order. Overlapping primitives, instances and draws share this ordering without an application barrier between them.

The pipeline enables this behavior with `VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT`. A legacy render pass also declares `VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT` on the subpass. Dynamic rendering uses the pipeline flag and the input-attachment index mapping from `VK_KHR_dynamic_rendering_local_read` or Vulkan 1.4. Metal orders native color inputs directly, so the same compiler path serves ordinary and ordered attachment reads.

Single-sample color reads retain the existing framebuffer-fetch path. For multisampled color attachments, the current sample is read from tile memory. Reads of a different sample retain the texture-read path and the synchronization required for that sample. Ordered attachment access does not create a dependency on unrelated samples or framebuffer locations.

Multisampled shaders using pixel interlock instead read attachment values from the coherent per-sample shadow in the [pixel-scope envelope](KOSMICKRISP_INTERLOCK.md). Explicit sample indices select the corresponding shadow value.

Input-attachment arrays select the mapped native color input at runtime, including non-uniform indices. Unmapped inputs, undefined color slots and depth/stencil inputs retain texture reads. Custom resolves read the original multisampled source through its texture descriptor; native color inputs refer to the separate resolve target.

Native ordered color inputs add no resource copies, per-draw allocations, command queues or CPU waits. They share the native framebuffer-fetch lowering.

`rasterizationOrderDepthAttachmentAccess` and `rasterizationOrderStencilAttachmentAccess` are false. Metal fragment functions have no corresponding depth or stencil attachment input; those input attachments use texture reads with explicit synchronization.

The extension orders attachment writes and input-attachment reads. General storage-buffer and storage-image operations require their own synchronization; this extension does not expose fragment shader interlock.

The dependency scopes are defined by [Vulkan color-blend pipeline flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineColorBlendStateCreateFlagBits.html). Metal color inputs and sample behavior are specified in [Metal Shading Language, sections 5.2.3.4 and 5.5](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).
