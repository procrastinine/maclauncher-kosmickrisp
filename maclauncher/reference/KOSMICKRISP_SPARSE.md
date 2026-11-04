# Sparse resources

MacLauncher KosmicKrisp implements Vulkan sparse binding and residency through
Metal placement sparse resources. Devices expose these capabilities when
Metal reports placement sparse support. Backing allocations use private heaps
and 64 KiB pages. Format queries and image creation validate the declared
Vulkan image usage, separately from internal transfer implementation flags.
Sparse memory requirements retain the validated aspect, granularity and flags.
Mutable compressed 3D sparse-resident images use standard-shaped residency for
block-texel views. Extended usage allows compatible raw views to supply their
format usage capabilities; host transfers remain unavailable for sparse images.
Sparse 2D-view compatibility retains its separate restriction.

`vkQueueBindSparse` maps, unmaps and replaces buffer pages and image tiles on
the Vulkan queue. The same mechanism supports aliases and opaque image binds.
GPU ResourceState barriers order mapping changes and subsequent accesses.
Submission completion retains mapping resources and retires old residency
metadata without a CPU wait.

Buffers, texel-buffer views, 2D images and 3D images support partial residency.
Unmapped reads return zero and writes are discarded. Buffer access lowering
checks residency for each scalar component and every page crossed by an
access. Buffer-device-address accesses use immutable, versioned sparse-range
tables, including pointers loaded through other buffers.

Shader sparse operations return both the value and residency status. Nonzero
base-mip image views use parent-range query views with adjusted LODs. Sampling
preserves filtering and view clamps; gather checks the four selected texels.
Sparse min/max filtering combines the residency status of its taps.

Set and heap buffer descriptors use the same 16-byte representation. A sparse
residency-record index occupies the last word; address arithmetic uses a
separate zero offset. Sparse sampling shares the contributing-tap footprint
with virtual sampler filtering. Residency includes every contributing tap;
colour and depth comparison use the original logical sampler parameters.

Sparse sampled views share rows by base mip and view level count. Each indexed
entry occupies 32 bytes: a native parameter pointer, physical border and
reduction facts, and black and white point-sampler resource IDs. Point taps
reuse the bounded canonical native states; no clamped query sampler is created
for each source sampler and view.

Native heap sampler metadata carries the full 64-bit source resource ID. The
sparse lookup matches that ID in the native metadata list. Each native
record occupies 64 bytes with 16-byte alignment: the next pointer at byte 0,
source ID at byte 8, 32-byte parameters at byte 16, physical facts at byte 48
and zero reserved words at bytes 52 through 63. Records use the existing sparse
residency and sampler lifetime.

Virtual heap samplers retain the original immutable parameter-record pointer
and use their pinned canonical index only for physical row facts. A canonical
native resource ID does not identify the original logical parameters. Direct
and embedded heap sampler mappings select their actual metadata records;
equivalent immutable records may have different addresses. Query rows are
allocated with sparse sampled views and retained through their existing
resource lifetime.

Sparse 3D views support sliced storage loads, stores, atomics and size queries.
Ordinary storage descriptors retain their slice offset and count. Sparse
nonzero-mip storage views use the tagged query-record encoding described in
the [descriptor layout](kosmickrisp-descriptor-layout.md). Storage descriptors
remain 16 bytes and sampled-image descriptors remain 64 bytes.

Image tile granularity and mip-tail layout are reported for each format and
image type. A nonstandard native 3D block shape is reported with its actual
granularity. Opaque binds cover the padded native mip-tail allocation.
Queue families with graphics, compute or transfer capability also expose
sparse binding when placement sparse resources are supported.

Enabling sparse features does not activate buffer guards in ordinary shaders.
The device records the first successful creation of a buffer or image with a
sparse-residency create flag. Pipelines then select a guarded variant on use.
Variants use the shared compiler, have separate cache keys and are serialized
beside the ordinary shader in application pipeline caches. Command buffers
recorded before the transition use the existing replay path at submission.
Binding-only sparse resources do not trigger this transition.
