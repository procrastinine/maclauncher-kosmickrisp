# Sampled-image descriptor layout

MacLauncher KosmicKrisp uses one 64-byte sampled-image descriptor layout.
Native image identifiers, sampler indices and sampling metadata occupy the
first 48 bytes. The final 16 bytes contain two auxiliary fields:

| Byte offset | Field | Size |
| --- | --- | --- |
| 48 | `aux_resource_id` | 8 bytes |
| 56 | `aux_word` | 8 bytes |

The field offsets and total descriptor size are fixed by compile-time assertions.
Image and sampler metadata retain their existing positions.

Storage-image descriptors occupy 16 bytes. The native texture identifier is
at byte 0; ordinary 3D views retain their slice offset at byte 8 and slice
count at byte 12. A sparse storage view with a nonzero base mip instead stores
a query-record address in bytes 8–15, with bit 63 identifying this encoding.
The sparse lowering decodes this address for sparse storage loads and for
slice-range queries used by 3D storage loads, stores, atomics and size queries.
An untagged range retains its original encoding and needs no record load.

For non-MS 1D/2D storage views with sparse query metadata, the primary word
is zero and the second word contains the tagged record address. Both words
zero identify a null descriptor. Normal texture operations resolve the record's
typed native identifier before constructing a Metal handle. Non-sparse native
identifiers retain every opaque bit and their existing inline encoding.

Sparse query records occupy 96 bytes. Sampled records begin with a native
texture identifier, base LOD and level count at offsets 0, 8 and 12.
Storage records begin with the parent texture identifier, slice offset and
slice count at the same offsets; their base LOD and level count occupy offsets
64 and 68. This prefix lets the shared sliced-3D lowering consume the selected
parent view without changing the ordinary descriptor encoding. Compile-time
assertions fix these offsets and sizes.

Normal plain and array texture identifiers occupy offsets 72 and 80. The wide
array query identifier occupies offset 88; the wide plain query identifier
remains at offset 0. Opposite-type full-mip views retain the same base layer
and layer count and live with the image view. Sparse sampled descriptors keep
independent native array identifiers at byte 48 and query metadata at byte 56.

The unsigned image minimum LOD at byte 20 contains a 14-bit numerical value.
Bit 14 (`KK_IMAGE_NATIVE_ARRAY_ALIAS_TAG`) identifies a native array texture
identifier in byte 48. Bit 15 (`KK_IMAGE_QUERY_RECORD_TAG`) identifies a readable
query-record address in byte 56; the record kind is independent of this tag.
The shared integer-LOD value mask is `KK_IMAGE_LOD_VALUE_MASK` (`0x3fff`).
The floating-point image minimum LOD at byte 18 and sampler fields retain their
existing representations.

Whole multi-plane views retain the native plane chain: plane 0 at byte 0,
plane 1 at byte 48 and plane 2 at byte 56. Their native-array alias tag is clear.
Individual plane views use the single-plane descriptor contract, including
array aliases. Descriptor-set plane N uses its primary identifier at 64*N.
Heap plane identifiers use byte 0,48 or56 while their flags remain at byte 20
of the root descriptor. Array sampling selects an alternate slot address
only when the native-array alias tag is set. Cube-face identifiers and
writable multisample tables retain their separate descriptor meanings.

## Standard-shaped sparse volumes

Standard-shaped 3D query records append an 8-byte logical-view pointer to the
96-byte sparse prefix. Their 104-byte stride leaves ordinary sampled and storage
descriptor sizes unchanged. The logical view selects a residency table and an
array of 16-byte canonical texture descriptors, one per logical 64 KiB binding.

Regular cells and packed mip-tail pages use the same cache keyed by bound device
memory, byte offset and tile shape. Packed levels use a block-linear byte offset
within the tail; that address selects the bound page and its block coordinates.
The original texel offset inside a compressed block is preserved for native
format decoding. Aliased bindings select the same canonical texture, while
rebinding a tail page changes the selected backing independently of other pages.
Native sparse backing retains every native tile touched by mapped logical cells.
The standard 3D block-shape property describes these logical cells; native Metal
tile dimensions do not change the application-visible binding granularity.

## Raw block layer entries

The maintenance6 `blockTexelViewCompatibleMultipleLayers` property reports
support for multi-layer raw block views. Original1D/2D arrays, cube faces and
regular3D depth-slice attachment views use the shared layer-selection contract.
Each uncompressed view contains one mip level and retains the original image's
size-compatible block contents.

A layered raw block-view record embeds the shared sparse query prefix and
points to ordinary sampled and storage descriptor arrays. Original1D arrays
use the same per-layer native2D-array aliases, with a physical height of one.
The common texture lowering supplies the unused native Y coordinate; storage
operations retain their logical1D layer coordinate and layer-count query. Regular3D
images created with2D-array compatibility select the original depth slices
through the existing image layout's layer stride and mip offsets. Their
per-slice raw aliases use this same layer table; sparse3D query tables retain
their separate representation. Sampled entries
retain the64-byte layout; storage entries retain the16-byte layout. Each array
ends with an all-zero null entry selected for rejected integer layers.

Each storage entry contains a plain2D native texture identifier at byte0 and
a2D-array native texture identifier at byte8. Both views refer to the same
physical image layer. Reads and writes select the array slot; the shared
image-alias atomic lowering selects the plain slot for the one-layer native
atomic path. The image view retains both native views until its normal
retirement. The prefix, sampler representation and native identifier bits
remain unchanged.

Cube and cube-array raw block views use the same per-face native2D-array
entries. Cube projection and face gradients use the shared cube-coordinate
utilities. Nearest and linear footprints select typed descriptor slot addresses;
seamless edge taps select the incident neighboring face, while nonseamless taps
retain sampler addressing and border behavior. Gather uses the four specified
component positions. Dimension queries report raw-block width/height and the
logical cube count rather than the physical one-layer native view. The raw
view contains one mip level. Derivative-based anisotropic sampling projects the
footprint ellipse onto the selected face and integrates centered line samples
along its major axis. The original sampler maximum bounds the sample count;
the common sampler accessor supplies that value and the original reduction
mode. Weighted averages and positive-contribution min/max reductions reuse the
same typed face taps, including sparse status combination. Explicit scalar LOD
and gather retain their ordinary footprints.

Multiview attachment initialization uses the actual retained view-mask bits,
including noncontiguous masks with layerCount1. Cached raster consumers bind
those actual attachment indices while using physical render-target index0.
Retained Mesh output is consumed through the shared Mesh begin/end callback
interface, before the next slice can overwrite the output cache.

## Typed buffer records

Typed buffer descriptors retain a 16-byte prefix followed by the existing
16-byte Sparse metadata. Explicit `VkBufferView` descriptors store their native
texture identifier at byte 0, the native texel prefix at byte 8 and the logical
element count at byte 12. Native RGB32 uses bit 31 of the prefix word and scalar
native coordinates. Native R64 integer views store a byte address and use the
64-bit buffer access path.

Address-only typed writers use `KK_TEXEL_BUFFER_RAW_ADDRESS` (bit 30) in the same
byte-8 word. The remaining low bits contain the existing `pipe_format`; byte 0
contains the requested full-width byte address, and byte 12 contains the logical
element count. Native prefix values are below 16, and the native RGB32 bit stays
separate. Image-array descriptors use a different view class and do not decode
this buffer mode.

Raw records use logical texel coordinates and Mesa format packing/unpacking for
loads, stores and fetches. Integer R32/R64 atomics retain their declared atomic
operation and access qualifiers. Bounds and Sparse residency guard byte access;
a byte address is never used as a native texture identifier. Native and raw
records can be consumed by the same participating pipeline. Buffer size queries
read the common logical element count.

The Sparse tail retains its residency-map address, byte prefix and coordinate
stride at offsets 16, 24 and 28. Raw RGB32 uses stride 12 and logical coordinates;
native RGB32 uses stride 4, the existing scalar-coordinate flag and a 12-byte
access span. R64 uses stride 8. The underlying resource and residency map follow
the selected buffer's legal-reader lifetime; copying descriptor bytes does not
retain the resource.

Address-only writers classify the exact requested range once through the existing
complete Sparse virtual-range directory. An ordinary range needs no public buffer
lookup. A Sparse range supplies its actual record index for bounded-buffer word 12
and its map/prefix for the typed tail. The underlying resource follows Vulkan's
valid-use lifetime; classification does not retain a public object or native owner.
Unknown or incomplete classification leaves destination bytes untouched. No native
view creation, cache insertion or allocation occurs during encoding.

Host block size and texels-per-block come from the Vulkan registry, including
planar formats and packed formats whose block extent differs from their texel
count. The host resolver preserves existing exact Pipe format identities without
adding shader support. Packed `B16G16R16G16_422_UNORM` uses the distinct
`U16Y16V16Y16_422_UNORM` Pipe identity for U0/Y0/V0/Y1 component order;
its Vulkan host geometry remains 8 bytes and one element per block. Encoding
bytes does not enable unsupported shader access.

Raw decoding is selected during the existing descriptor pass for actual
Heap/descriptor-buffer shader binding flags. Those flags already participate in
the common shader key. Ordinary descriptor-set shaders use the native visitor
and emit no raw-format dispatch.

## Embedded sampler bindings

Embedded heap samplers use immutable device-owned address arrays. The existing
root set-address union contains the resource and sampler heap addresses followed
by `heap_embedded_samplers`, indexed by the original Mesa shader stage. The
32-word union and all later root offsets retain their size and position.
Ordinary descriptor-set addresses remain in CPU state; heap aliases are applied
only to uploaded snapshots.

A shader with one embedded binding stores its metadata address directly in
the stage root word. Multiple bindings use an immutable address array; the
instruction loads the stage array pointer and selects its binding ordinal. Device addresses remain runtime
data. Shader source contains the stage offset and ordinal, allowing identical
source across devices and processes. The arrays and metadata use the existing
device-retained sampler arena; shader binding refreshes the ordinary root
snapshot when its stage array changes.

## Original logical sampler parameters

Every logical sampler retains one immutable32-byte,16-aligned parameter record
in the existing device metadata arena. The record contains original fp32 LOD
and anisotropy values, comparison state and reduction mode. It remains resident
until device teardown, including after sampler destruction or descriptor copies.

Sampler filter bit4 (`KK_SAMPLER_FILTER_PARAMS_POINTER`) identifies the full
unsigned GPU address packed at bytes12,14,16 and22, in successive16-bit chunks.
Bit3 independently selects virtual sampling. Native samplers with bit4 set keep
native sampling and read logical metadata through the common parameter accessor;
untagged native descriptors retain their numeric half-float representation.
Image fields18/20, border24–39, auxiliary48/56, descriptor sizes and root sizes
are unchanged. All sampler planes share one logical parameter address.
