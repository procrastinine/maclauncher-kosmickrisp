# Single-layer image descriptor aliasing

Single-layer 1D and 2D image views created for descriptor aliasing retain native Metal views for both plain and array shader declarations. Sampled accesses select the view matching the shader, including multisampled 2D accesses. Storage reads, writes and queries use the same selection. Base mip and array-layer ranges remain relative to the Vulkan view.

Array atomics on a single-layer alias use its plain native view and preserve the layer-zero bounds check. This also supports atomics on buffer-backed linear images. Ordinary multi-layer arrays retain native array atomic access.

Sampled-image descriptors remain 64 bytes and storage-image descriptors remain 16 bytes. Array handles use the existing secondary storage word. Sparse views use the indirect query-record form described below; buffer and 3D metadata retain their separate view-class interpretation. Cube descriptors retain their separate auxiliary interpretation. Alternate native views are created only for images requesting single-layer descriptor aliasing.

## Vulkan Maintenance11

`VK_KHR_maintenance11` supplies the single-layer descriptor aliasing flag and the shared queue and rasterization contracts. All queue families report unit minimum image-transfer granularity; byte-aligned buffer/image transfer offsets retain texel-block alignment. Concurrent sharing accepts a single queue family. Without explicit static or dynamic depth-clip state, clipping follows the inverse of depth clamping.

The shader-object independent-set and task-free pipeline-layout compatibility rules apply when `VK_EXT_shader_object` is exposed. The current profile does not expose shader objects.

## Sparse queries and descriptor heaps

A sparse 1D or 2D storage view with query metadata uses a zero primary word and a tagged query address in its secondary word. The query record retains the actual plain and array view IDs at offsets72 and80 and the array full-mip query ID at88; its existing prefix remains unchanged. Null descriptors contain two zero words. Readers distinguish the complete pair before selecting the address of a native ID, so opaque native-ID bits remain intact. Descriptor-heap mappings and ordinary descriptor sets share these writers and accessors.

Image-view lifetime retains both native full-mip query types for alias-enabled sampled and storage views. Sparse queries choose the native type matching the shader; atomics compare IDs loaded through the resolved normal-view addresses.

## Multisample representation ownership

An image with more than one sample and storage usage owns the writable multisample representation, including sampled-only narrowed views. The single-layer alias writers leave its storage sample/layer words and sampled table metadata untouched. Native multisample aliases retain their typed native handles and a zero sampled auxiliary metadata word. Multisample storage conversion resolves its sample-table handle before ordinary native handle selection.
