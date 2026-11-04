# Multisample storage images

MacLauncher KosmicKrisp implements `shaderStorageImageMultisample` for supported two-sample and four-sample color images. Image-format queries determine the available sample counts for each format, usage and creation flag combination. The storage-image sample-count limit contains only counts supported by the Metal device, up to four.

A multisample image with `STORAGE` usage has a native multisample resource and one single-sample array texture for each sample. Each array retains the image's full logical layer range. These resources occupy the image's existing `VkDeviceMemory` binding, and the memory requirements include their size and alignment. Single-sample images and multisample images without storage usage retain their existing representation. Implicit multisampled-render-to-single-sampled attachments also retain their native representation.

## Shader access and views

Storage loads, stores and atomics select the array texture for the requested sample. Sample selection preserves the access type and ordinary logical coordinates. A view's base layer is applied when its native sample views are created; shader coordinates remain relative to the view. Sample and layer bounds are checked before a resource-table entry is loaded. Invalid sample or layer accesses do not load a resource ID; stores and atomics have no memory effect. With `robustImageAccess2`, rejected reads use the logical format's zero and missing-component defaults, with the view component mapping applied to sampled results. Null descriptors return all zero components.

Sampled multisample operations select their representation from the image descriptor at execution time. A table address with valid sample and layer metadata selects the single-sample array path. Native records retain the native multisample path, including native array IDs in the auxiliary slot. This also allows one shader to access ordinary native multisample images and writable multisample images through different descriptors.

Storage and texture sample-count queries return the logical sample count. Size queries return the original extent and view layer count. The sample count does not expand the logical array size or reduce the array-layer limit.

Input attachments and native attachment operations use the native multisample resource. Traditional input-only subpass attachments are prepared separately from color attachments, including normal and imageless framebuffers.

## Rendering and synchronization

The per-sample arrays hold storage contents. Native attachment contents are loaded before a physical render encoder starts and published after that encoder finishes. Copies use Mesa's shared per-sample image-copy shaders and the existing encoder fences. Unsigned integer views of equal texel bit length preserve packed fields, normalized endpoints and floating-point payload bits during conversion.

Conversions cover the rendered view layers. A native store preserves samples when an attachment also resolves. Fragment shaders that write storage without color outputs do not publish stale native color contents over their storage writes. The ordinary Vulkan barriers and queue dependencies order storage, sampled, attachment and alias accesses, including accesses that retain the `GENERAL` layout.

The command buffer preserves application rendering and pipeline state around conversion work. Physical encoder boundaries govern publication across suspension and resumption. Mixed passes requiring writable-multisample conversion use persistent backing before memoryless attachments are selected.

Images with matching alias creation parameters and memory bindings share their sample storage. No global image dirty state or descriptor rewrite represents content visibility.

## Descriptor representation and lifetime

The storage descriptor remains 16 bytes. The sampled descriptor remains 64 bytes. Only writable multisample image records use the fields below.

| Record | Offset | Contents |
| --- | ---: | --- |
| Storage | 0 | 64-bit GPU address of the sample resource table |
| Storage | 8 | 32-bit sample count, in samples |
| Storage | 12 | 32-bit view layer count, in layers |
| Sampled | 0 | Primary native multisample resource ID |
| Sampled | 48 | 64-bit GPU address of the same sample resource table |
| Sampled | 56 | Low 32 bits: sample count; high 32 bits: view layer count |

A view owns an immutable table with one 64-bit native resource ID per sample. Entry `s` identifies sample `s`'s single-sample 2D-array view over the original base layer and layer count. Layer coordinates do not enter the table index. An immutable eight-byte footer follows the sample IDs: a 32-bit missing-alpha flag for storage reads and a 32-bit four-channel default-one mask for sampled reads. The mask reflects the logical view format and component mapping. A rejected read may load this bounded metadata only after checking the pointer, supported sample count and positive layer count. Valid reads, stores, atomics and queries do not load the footer. The view strongly owns these native views and its table BO through their legal GPU lifetime. Retained internal views carry their own sample count so their retirement does not depend on a destroyed parent image.

Existing descriptor writers share the same producer for ordinary sets, push descriptors, templates and descriptor copies. Heap writers use that producer as well. Image and sampler records retain separate auxiliary meanings; combined-sampler updates preserve the image fields. Existing allocation and residency mechanisms retain the table and its actual backing resources. Table entries and resource identities remain fixed while texture contents change.

Buffer-image descriptors, including RGB32 metadata and the R64 buffer-address route, retain their existing meanings. Sparse multisample images remain excluded by format queries and image creation checks. Sparse auxiliary decoding excludes the original multisample dimension before the conversion to ordinary array operations. Cube-compatible and multi-planar multisample restrictions remain in force.
