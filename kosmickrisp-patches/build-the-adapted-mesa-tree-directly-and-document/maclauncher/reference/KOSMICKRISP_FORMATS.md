# KosmicKrisp image and buffer formats

MacLauncher KosmicKrisp supports additional Vulkan formats through native Metal images and shared view, transfer and resolve paths.

## Integer multisampling

R32, R32G32 and R32G32B32A32 UINT and SINT images support the sample counts reported by the Metal device when `supports32BitMSAA` is available. Integer color resolves select sample zero through Mesa's meta resolve implementation. This applies to rendering resolves and explicit image resolves, including partial regions and array layers. Integer formats do not use an averaging resolve.

Multisampled color attachments support dynamic rendering local reads. Framebuffer dependencies preserve the individual samples between rendering and subsequent input-attachment reads. Multisampled storage images are unavailable.

## RGB32 images and buffer views

R32G32B32 SFLOAT, UINT and SINT formats support uniform and storage texel buffers. A view uses consecutive native R32 elements, with three component reads or writes for each logical texel and an alpha value of one on reads. View sizes and offsets remain expressed in RGB texels. Bounds checks apply before the native alignment prefix is added; empty views report zero elements. Storage accesses support unformatted loads and stores.

Optimal-tiling RGB32 images support sampling, storage access, clears and copies. Their native backing uses RGBA32 with a hidden fourth component. Views return one for alpha, and transfers preserve the three RGB words exactly. Buffer copies retain Vulkan's packed 12-byte texel layout, including row padding and subresource offsets. The conversion uses a staging allocation bounded to four MiB per copy region. Image copies share the normal copy path, including copies between RGB32 numeric types and between volumes and arrays.

RGB32 images do not support linear tiling, color attachments or host image transfers. Other typed formats retain their native representations and the same descriptor layout.

## Alpha and packed formats

A8 UNORM supports uniform and storage texel buffers, sampled images, storage images and color attachments with blending. Alpha remains in its logical channel through blend equations and color-write masks.

The packed 16-bit formats R4G4B4A4, B4G4R4A4, A4R4G4B4 and A4B4G4R4 use Metal's ABGR4 layout with the corresponding channel permutation. R5G6B5, B5G6R5, R5G5B5A1, B5G5R5A1, A1R5G5B5 and A1B5G5R5 use Metal's corresponding packed layouts. These formats support uniform and storage texel buffers, sampled images, storage images and color attachments with blending. Storage accesses support unformatted loads and stores. Format queries report the available sample counts and operations. Buffer views use the same channel mapping as images.

B8G8R8A8 SNORM, UINT and SINT support sampled images through RGBA8 views with a red/blue channel swap. They do not support color attachments or storage images. Fixed opaque-black borders use the shared sampler correction without requiring the optional custom-border-color feature.
