# 64-bit storage image atomics

MacLauncher KosmicKrisp supports VK_EXT_shader_image_atomic_int64 and the shaderImageInt64Atomics feature on Apple GPU family 8 and later (M2 and later). Native unsigned 64-bit texture minimum/maximum operations provide the publication primitive for the complete operation set. R64_UINT and R64_SINT storage images preserve all 64 bits for atomic addition, subtraction, signed and unsigned minimum/maximum, bitwise and/or/xor, exchange, compare-exchange, and atomic loads/stores. Returning operations supply the complete previous value.

Both formats use eight bytes per texel. Optimal storage images support 1D, 2D, arrays, cube storage views and 3D images through the shared image paths. Linear images follow the supported 2D layout and alignment rules. Storage texel-buffer views preserve their byte offsets, bounds and overlapping aliases. Image views preserve mip and array-layer aliases.

The format features include storage, storage atomics and transfers. R64 formats have no color-attachment, filtering or multisampled-storage capability. Compatible R32G32_UINT views can sample the two 32-bit words of a mutable image with extended usage. This supports sampler-feedback bitsets and their decode operations without converting the stored bits.

Texture atomic operations share the device's full-width atomic synchronization with buffer and shared-memory operations. Signed operations compare signed 64-bit values; arithmetic overflow wraps at 64 bits. Contending operations serialize at a texel. Unrelated shaders have no image-atomic helper or texture-lock work.

The extension's sparse-image atomic feature is independent of ordinary storage image atomics and follows the available sparse-image support.
