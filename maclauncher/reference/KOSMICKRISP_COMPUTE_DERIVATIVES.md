# Compute shader derivatives

MacLauncher KosmicKrisp supplies `VK_KHR_compute_shader_derivatives` for linear derivative groups in compute shaders. Four consecutive local invocations form a derivative group. Fine and coarse derivatives use Metal quad operations; implicit texture samples use explicit gradients while retaining anisotropic filtering. Compute LOD queries include sampler bias, mipmap filtering mode, sampler LOD bounds and the image view's available mip levels.

The sampled-image descriptor remains 64 bytes. Sampler mipmap mode and query bias occupy existing padding, alongside the unchanged image minimum-LOD fields. Shaders without the linear derivative execution mode use the existing shader path. Quad derivative groups and mesh/task derivatives are not exposed.

The adaptation is the `compute-derivatives` capability in the [source catalog](../../src/kosmickrisp/). It uses Mesa's quad intrinsics and dimension-aware gradient-to-LOD lowering. The [Vulkan sampling specification](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-lod-query) defines the LOD query result.
