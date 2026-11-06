# KosmicKrisp source adaptations

MacLauncher KosmicKrisp applies the following source adaptations to the Mesa revision selected by [kosmickrisp-source.json](../kosmickrisp-source.json). Mesa references without a fixed revision use that source pin. Capabilities implement missing Vulkan features, extensions, properties or limits. Corrections repair behavior already exposed by upstream. Compatibility adaptations provide a consumer interface. The [ordered registry](registry.mjs) composes one module per adaptation through the existing source projection. [patches.json](patches.json) records capabilities, affected contracts, consumers, upstream relationships, replacement conditions and verification targets.

| Module | Kind | Behavior | Existing paths |
| --- | --- | --- | --- |
| [constant-matrix-coordinate-ramp](constant-matrix-coordinate-ramp.mjs) | correction | Copy exact immutable coordinate defaults during recording initialization | modifying |
| [retired-replay-pool-walk](retired-replay-pool-walk.mjs) | correction | Omit the empty pool traversal after individual replay retirement | modifying |
| [dynamic-depth-bind-once](dynamic-depth-bind-once.mjs) | correction | Bind combined dynamic depth and pipeline state once | modifying |
| [replay-autorelease-scope](replay-autorelease-scope.mjs) | correction | Share a lexical replay pool and preserve standalone bridge pools | modifying |
| [fresh-render-color-attachments](fresh-render-color-attachments.mjs) | correction | Populate active fresh color slots and retain clearing on remap | modifying |
| [direct-compute-threadgroups](direct-compute-threadgroups.mjs) | correction | Encode direct Vulkan dispatches as native complete threadgroups | modifying |
| [command-upload-pool-count](command-upload-pool-count.mjs) | correction | Initialize and clear the upload BO reuse count with its list | modifying |
| [initial-command-reset](initial-command-reset.mjs) | correction | Skip repeated flag-free initial command-buffer cleanup | modifying |
| [compute-pipeline-bind-dirty](compute-pipeline-bind-dirty.mjs) | correction | Preserve repeated ordinary compute pipeline bindings; invalidate for new encoders, shaders and internal work | modifying |
| [argument-root-bind-dirty](argument-root-bind-dirty.mjs) | correction | Omit unchanged root addresses on the same argument table; retain initial binds after reset or table replacement | modifying |
| [empty-dependency-barrier](empty-dependency-barrier.mjs) | correction | Leave the current encoder intact for an empty dependency with zero flags and no extension chain. All memory barriers, execution dependencies and ownership transfers retain their existing handling. | modifying |
| [descriptor-bind-dirty](descriptor-bind-dirty.mjs) | correction | Repeated single-set binds without dynamic descriptors skip the binding loop. Keep descriptor roots clean across identical set and dynamic-offset binds while preserving push descriptor invalidation and changed root addresses. | modifying |
| [residency-commit-dirty](residency-commit-dirty.mjs) | correction | Persistent nonpageable residency is requested once per device. Track execution residency membership changes under the existing mutex and omit unchanged Metal residency-set commits. Residency requests, queue attachment and priority residency retain their existing behavior. | modifying |
| [command-allocator-lifetime](command-allocator-lifetime.mjs) | correction | Recorded native storage is released before allocator reset. Native buffer and allocator retirement across reset, trim and destruction. Keep native command allocators exclusive to executable Vulkan command buffers, including split mesh segments, and reset and return their storage only when the recording is legally reset or destroyed. | modifying |
| [host-deferred-submit](host-deferred-submit.mjs) | correction | Host continuation points with on-demand Mesa queue submission and committed semaphore dependencies | modifying |
| [fragment-shading-rate](fragment-shading-rate.mjs) | capability | Linked coarse fragment execution and retained fine replay through a shared lazy artifact cache; Vulkan shading-rate features remain unavailable | additive |
| [timestamp-counter-heaps](timestamp-counter-heaps.mjs) | correction | Timestamp pools spanning native counter heaps with shared linear results | modifying |
| [subgroup-graphics-stages](subgroup-graphics-stages.mjs) | capability | Real subgroup operations in native VS and Poly graphics stages | modifying |
| [multiview-poly](multiview-poly.mjs) | capability | Per-view GS and tessellation replay, layers and query accounting | modifying |
| [multiview-draw-records](multiview-draw-records.mjs) | correction | Preserve complete direct multi-draw records in each per-view replay | modifying |
| [shader-long-vector](shader-long-vector.mjs) | capability | [Long shader vectors](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_LONG_VECTORS.md) through 1024 components, shared scalar arithmetic and memory lowering | modifying |
| [sampler-emulation-helpers](sampler-emulation-helpers.mjs) | correction | Share sampler reduction and border paths across matching sample operations | modifying |
| [alu-temporary-declarations](alu-temporary-declarations.mjs) | correction | Omit redundant initialization of fully assigned ALU temporaries; retain zero initialization for guarded reads | modifying |
| [transform-feedback](transform-feedback.mjs) | capability | Poly transform feedback and generated primitive queries | modifying |
| [custom-border-colors](custom-border-colors.mjs) | capability | Enable the existing custom border color implementation | modifying |
| [polygon-clip-varyings](polygon-clip-varyings.mjs) | correction | Scalar clip and cull varyings in generated polygon vertex shaders | modifying |
| [polygon-lines](polygon-lines.mjs) | capability | Native Metal line polygon state | modifying |
| [attachment-access](attachment-access.mjs) | capability | Native rasterization-ordered color reads with scalar, multisample and dynamic-array input attachments | modifying |
| [fragment-interlock](fragment-interlock.mjs) | capability | Sample-scoped storage-resource interlock with physical-address ordering | modifying |
| [multisampled-render-to-single-sampled](multisampled-render-to-single-sampled.mjs) | capability | [Implicit multisample attachments](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_MSRTSS.md), load replication and single-sample resolves | modifying |
| [attachmentless-multisample](attachmentless-multisample.mjs) | capability | Change sample count between attachmentless draws | modifying |
| [texel-buffer-alignment](texel-buffer-alignment.mjs) | capability | Single-texel logical buffer views over aligned Metal textures | modifying |
| [conservative-rasterization](conservative-rasterization.mjs) | capability | [Conservative triangle coverage](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_CONSERVATIVE_RASTERIZATION.md) with original-corner interpolation and input masks | additive |
| [polygon-points](polygon-points.mjs) | capability | Capture, clip and rasterize triangle polygons as points | modifying |
| [geometry-streams](geometry-streams.mjs) | capability | Select and lower geometry rasterization streams | modifying |
| [stream-query-counters](stream-query-counters.mjs) | capability | Independent complete 64-bit generated and written reports | modifying |
| [xfb-buffer-room](xfb-buffer-room.mjs) | capability | Use unsigned wide arithmetic for transform feedback capacity | modifying |
| [generated-query-no-gs](generated-query-no-gs.mjs) | capability | Count generated primitives without a geometry shader | modifying |
| [dynamic-passthrough-topology](dynamic-passthrough-topology.mjs) | capability | Select passthrough adjacency vertices from actual draw topology | modifying |
| [dynamic-multisample](dynamic-multisample.mjs) | capability | [Draw-time raster samples, sample mask and alpha-to-coverage](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_MULTISAMPLE.md) | modifying |
| [vertex-input-dynamic-state](vertex-input-dynamic-state.mjs) | capability | [Draw-time vertex formats, bindings, strides and divisors](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_VERTEX_INPUT.md) | modifying |
| [depth-bias-control](depth-bias-control.mjs) | capability | Exact, FLOAT and FORCE_UNORM bias after polygon clipping | modifying |
| [memory-types](memory-types.mjs) | capability | [Private Metal device memory](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-memory.md) with shared host-visible storage | modifying |
| [allocation-residency-references](allocation-residency-references.mjs) | correction | Retain one native allocation registration across imported memory aliases | modifying |
| [memory-priority](memory-priority.mjs) | capability | [Priority hints and execution residency](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-memory.md) with optional idle retention | modifying |
| [image-compression-control](image-compression-control.mjs) | capability | [Default and disabled image compression](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-memory.md) in the shared layout path | modifying |
| [image-alignment-control](image-alignment-control.mjs) | capability | [Native minimum image layouts](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-memory.md) with maximum-alignment hints | additive |
| [heap-base-alignment](heap-base-alignment.mjs) | correction | [Aligned logical heap bases](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-memory.md) with shared placement translation and retained import regions | modifying |
| [extra-queue-families](extra-queue-families.mjs) | capability | [Independent compute and transfer queues](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_QUEUES.md) with shared-event synchronization | additive |
| [atomic-int64](atomic-int64.mjs) | capability | Full-width buffer and shared integer atomics with a device lock table | additive |
| [image-atomic-int64](image-atomic-int64.mjs) | capability | [Full-width storage image atomics](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_IMAGE_ATOMIC_INT64.md), buffer aliases and sampler-feedback bitsets | additive |
| [cooperative-matrix-fp8](cooperative-matrix-fp8.mjs) | capability | [16x16 subgroup matrix properties, byte and workgroup addressing, GPU FP8 conversions](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_MATRIX_TILES.md) | additive |
| [cooperative-matrix-tile](cooperative-matrix-tile.mjs) | capability | [FP16/FP32 subgroup matrix compiler operations](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_MATRIX_TILES.md) with opaque Metal fragments | additive |
| [compute-derivatives](compute-derivatives.mjs) | capability | Linear compute derivatives, implicit texture gradients and LOD queries | additive |
| [required-compute-subgroup-size](required-compute-subgroup-size.mjs) | capability | [Explicit wave32 compute subgroups](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_REQUIRED_SUBGROUP_SIZE.md) | additive |
| [float-controls](float-controls.mjs) | capability | Apple float controls, 128 dynamic descriptors and 8192 allocations | modifying |
| [fp32-denorm-preserve](fp32-denorm-preserve.mjs) | capability | Explicit FP32 gradual underflow, integer RTE packing and fused TwoProduct arithmetic | modifying |
| [shader-quad-control](shader-quad-control.mjs) | capability | Fragment quad control and active-invocation quad votes | modifying |
| [pipeline-statistics](pipeline-statistics.mjs) | capability | Eleven core statistics from actual shader and Poly producers | modifying |
| [helper-side-effects](helper-side-effects.mjs) | correction | Guard memory writes after fragment demotion while preserving termination exits | modifying |
| [zero-initialize-allocation-clear](zero-initialize-allocation-clear.mjs) | correction | Honour the allocation flag with a shared CPU clear or completed private GPU fill | modifying |
| [zero-initialize-device-memory](zero-initialize-device-memory.mjs) | capability | Zeroed allocations and preserving initial image layouts | additive |
| [unused-attachments](unused-attachments.mjs) | capability | Cached Metal pipeline formats and discarded writes for unused dynamic-rendering attachments | modifying |
| [unused-attachments-remap](unused-attachments-remap.mjs) | correction | Preserve live color outputs when unused attachment map entries alias their locations | modifying |
| [inactive-framebuffer-reads](inactive-framebuffer-reads.mjs) | correction | Specialize absent framebuffer-read inputs while preserving live blending and local reads | modifying |
| [image-view-min-lod](image-view-min-lod.mjs) | capability | Fractional image-view minimum LOD through Mesa descriptor lowering | modifying |
| [image-sliced-view-of-3d](image-sliced-view-of-3d.mjs) | capability | Relative Z ranges for 3D storage views with full-depth sampled access | modifying |
| [descriptor-heap](descriptor-heap.mjs) | capability | Shared descriptor heaps, byte-offset storage views and native sampler IDs | additive |
| [storage-buffer-alignment](storage-buffer-alignment.mjs) | correction | Four-byte storage-buffer offsets and under-aligned scalar accesses | modifying |
| [push-data](push-data.mjs) | capability | 4096-byte constants and 256 descriptors with layout-sized uploads | modifying |
| [metal-present-scaling](metal-present-scaling.mjs) | capability | Metal presentation scaling and gravity | modifying |
| [headless-surface](headless-surface.mjs) | capability | Native headless swapchains on macOS | modifying |
| [incremental-present](incremental-present.mjs) | capability | Incremental presentation region hints | modifying |
| [metal-drawable-count](metal-drawable-count.mjs) | capability | Double or triple buffered Metal swapchains | modifying |
| [integer-msaa](integer-msaa.mjs) | capability | 32-bit integer multisampling with sample-zero resolves | modifying |
| [rgb32-texel-buffers](rgb32-texel-buffers.mjs) | capability | RGB32 uniform texel buffer views | modifying |
| [sampled-formats](sampled-formats.mjs) | capability | Alpha, packed 16-bit and swizzled BGRA sampled images | modifying |
| [sampler-minmax](sampler-minmax.mjs) | capability | Shader sampler minimum and maximum reduction | modifying |
| [extended-flags](extended-flags.mjs) | capability | [64-bit image create and usage flags](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_EXTENDED_FLAGS.md) with the shared image runtime | modifying |
| [fragment-shader-barycentric](fragment-shader-barycentric.mjs) | capability | [Barycentric weights and original per-vertex inputs](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-barycentric.md), including geometry adjacency | modifying |
| [opaque-black-comparison](opaque-black-comparison.mjs) | correction | Preserve opaque-black depth comparison borders | modifying |
| [border-interpolation](border-interpolation.mjs) | correction | Preserve unaffected custom-border sample components | modifying |
| [external-memory-fd](external-memory-fd.mjs) | capability | [Owning POSIX memory handles](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_EXTERNAL_MEMORY.md) for buffers and compatible external color images | additive |
| [external-memory-fd-residency](external-memory-fd-residency.mjs) | correction | Shared BO residency registration and allocation priority for external FD memory | modifying |
| [external-memory-imports](external-memory-imports.mjs) | correction | Reject invalid heap and host imports before Metal access | modifying |
| [zero-extent-rendering](zero-extent-rendering.mjs) | correction | Clamp attachmentless Metal dimensions to one | modifying |
| [clc-variant-passes](clc-variant-passes.mjs) | correction | Run compiler workaround and optimization passes on each variant | modifying |
| [precompiled-indirect-dispatch](precompiled-indirect-dispatch.mjs) | correction | Use thread dimensions for precompiled indirect-local grids | modifying |
| [empty-texel-views](empty-texel-views.mjs) | correction | Valid empty whole-size views without a Metal texture | modifying |
| [event-dependencies](event-dependencies.mjs) | correction | Apply event dependencies through shared pipeline barriers | modifying |
| [pipeline-statistics-compatibility](pipeline-statistics-compatibility.mjs) | correction | Preserve fragment helper semantics and inactive statistics storage | modifying |
| [shader-feature-key](shader-feature-key.mjs) | correction | Shared compiler-assigned shader feature bits for pipeline statistics and sampler reduction | modifying |
| [metal-wsi-queue](metal-wsi-queue.mjs) | correction | Separate drawable waits from application submissions | modifying |
| [full-varying-interface](full-varying-interface.mjs) | correction | [Full vertex-fragment interfaces](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-varyings.md) with shared native varying packing | modifying |
| [sampled-descriptor-layout](sampled-descriptor-layout.mjs) | correction | [Fixed auxiliary words in 64-byte sampled-image descriptors](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-descriptor-layout.md) | layout |
| [wine-entry-points](wine-entry-points.mjs) | compatibility | Direct Wine entry points and promoted command lookup | additive |
| [rgb32-images](rgb32-images.mjs) | capability | RGB32 images and exact transfer conversion | modifying |
| [packed-alpha-formats](packed-alpha-formats.mjs) | capability | Alpha-only blending and packed 16-bit attachments | modifying |
| [multisample-local-read](multisample-local-read.mjs) | capability | Dynamic multisample color input attachments | modifying |
| [tessellation-indirect-grids](tessellation-indirect-grids.mjs) | fix | Preserve vertex, patch and instance ownership in indirect tessellation | modifying |
| [canonical-per-vertex](canonical-per-vertex.mjs) | capability | [Independent-stage per-vertex interfaces](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-independent-interfaces.md) with shared fixed-stride transport | modifying |
| [compile-shader-options](compile-shader-options.mjs) | compatibility | [Shared compiler options](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_COMPILE_OPTIONS.md) with null defaults | additive |
| [prefix-count-publication](prefix-count-publication.mjs) | fix | Fence device-memory prefix totals before tessellation index allocation and draw publication | modifying |
| [device-generated-commands](device-generated-commands.mjs) | capability | GPU-generated state and draw or dispatch commands | additive |
| [dgc-linked-table-buffers](dgc-linked-table-buffers.mjs) | fix | Per-command linked-stage wrapper buffers and separate interlock guard through shared argument slots | modifying |
| [soft-fp64](soft-fp64.mjs) | capability | [Software binary64 shader arithmetic](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_FP64.md) and independent float controls | modifying |

| [writable-msaa](writable-msaa.mjs) | capability | [Multisample storage images](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_WRITABLE_MSAA.md) with per-sample storage, logical view defaults and native attachment publication | additive |

LINE and POINT together supply non-solid polygon fill. Polygon points retain the fixed 1.0 size declared by the selected profile. The adaptations also change shared descriptors, shader compilation and caching, Poly parameter layouts, render encoder state and query handling. Existing native geometry and tessellation remain supplied by Mesa.

[Descriptor heaps](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/kosmickrisp-descriptor-heaps.md) describes heap mappings, descriptor layouts and storage-buffer alignment.

[Image views](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_IMAGE_VIEWS.md) describes minimum LOD and sliced 3D storage ranges.

GPU-side root word 0 holds the int64 lock-table address when int64 atomics are enabled; no module may read it as the root self-address.

An adaptation can be removed with its registry entry when upstream supplies its complete behavior and dependent producer/consumer contracts. Shared XFB and polygon contracts require a coordinated replacement. Upstream refresh reports compare the recorded replacement conditions and leave source selection unchanged.

| [device-address-commands](device-address-commands.mjs) | capability | [Address-based transfers and bindings](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_DEVICE_ADDRESS_COMMANDS.md) with shared copy, fill and update implementations | modifying |


[Device-generated commands](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_DEVICE_GENERATED_COMMANDS.md) describes token preprocessing, GPU command encoding and indirect draw limits.

The prefix-count publication correction adds the global memory fence at `src/kosmickrisp/libkk/kk_geometry.cl:23`. Related count-publication sites are recorded with source locations in the module’s `audit` entry in [patches.json](patches.json): geometry and polygon prefix scans, Poly subgroup scans, restart and adjacency builders, XFB counters, indirect predicates, query reductions and tessellator count/coordinate allocation. Only the tessellation scan consumes another lane’s global result before its dispatch ends.

| [additional-graphics-family](additional-graphics-family.mjs) | capability | A second graphics family with independent native submission and shared resource synchronization | additive |

Placement sparse resources use 64 KiB private heap pages, Metal texture view pools for buffer views, native residency queries and per-layer mip-tail mapping. Native granularity is reported for each format and texture type.
| [whole-pass-transient-attachments](whole-pass-transient-attachments.mjs) | capability | [Render continuations and lazy transient storage](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_TRANSIENT_ATTACHMENTS.md) | modifying |

| [maintenance11](maintenance11.mjs) | capability | [Single-layer image descriptor aliasing](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_IMAGE_ALIASING.md) with native plain and array views | modifying |


- `native-block-alias`: shared native texture alias construction and cleanup over caller-owned image planes.

| [geometry-raster-bounds](geometry-raster-bounds.mjs) | fix | [Bound generated geometry raster vertex, primitive and instance IDs](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_GEOMETRY_BOUNDS.md) | modifying |

The [command depth/stencil interface](https://github.com/procrastinine/maclauncher-builds/blob/main/docs/KOSMICKRISP_DEPTH_STENCIL_STATE.md) exposes the selected native state and command-owned phase variants to internal encoders.

| [dgc-layered-producers](dgc-layered-producers.mjs) | fix | Prepare generated graphics once before shared layered raster readers | modifying |
