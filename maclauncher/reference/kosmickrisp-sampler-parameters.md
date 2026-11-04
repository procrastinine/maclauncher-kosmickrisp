# Immutable sampler parameter storage

Sampler parameter records use the shared kk-sampler-params.h declaration and occupy 32 bytes with 16-byte alignment. The allocation API stores the complete record in the same resident 64 KiB pages used for 192-byte sampler metadata. Pages remain allocated until valid device teardown, preserving addresses referenced by copied and embedded metadata. The optional CPU allocation result and host sampler params_cpu field alias the same initialized arena bytes as params_gpu_address. The CPU alias remains NULL until finalization succeeds and shares the record's device lifetime.

kk_sampler_params_alloc_locked requires descriptor_heap_lock to be held by its caller. kk_sampler_params_alloc acquires and releases that lock. Both allocation entries require the native sampler mutex to be released. Sampler constructors carry an explicit kk_sampler_params_lock_context independent of native sampler indexing.

kk_sampler_params_pack_gpu_address stores a 64-bit GPU address in the sampler metadata halfwords at bytes 12, 14, 16 and 22. Packing follows ordinary sampler metadata preparation. The operation publishes bit4 of sampler_filter_bits after all address chunks are stored. Native and virtual samplers use this pointer-presence bit independently of the sampling-dispatch bit3. Image, border and auxiliary fields are preserved. Untagged native metadata retains its numeric half-float representation.

The common sampler constructor finalizes native and virtual logical parameters through one path before publishing the sampler. Record initialization uses the reduction mode already decoded by the Vulkan sampler initializer. Its caller supplies the lock context for ordinary creation or creation while the descriptor heap lock is held. Descriptor preparation uses this finalized state for classic sets, immutable samplers, pushed descriptors, update templates, descriptor copies, direct sampler heaps and embedded samplers.

Virtual samplers set bit 3 of sampler_filter_bits. Bits 0 and 1 retain the plane's original minimum and magnification filters; bit 2 retains the non-seamless cube setting. All reconstruction and chroma planes belonging to one logical sampler use one immutable parameter address. Each plane retains its own filter metadata. Byte42 retains its existing software-reduction dispatch meaning; the original logical reduction mode is stored in the immutable record.

The shared parameter declaration supplies the following 32-byte record:

| Byte offset | Field | Type |
| --- | --- | --- |
| 0 | Minimum LOD | float32 |
| 4 | Maximum LOD | float32 |
| 8 | LOD bias | float32 |
| 12 | Maximum anisotropy | float32 |
| 16 | Mipmap-linear and anisotropy-enable flags | uint32 |
| 20 | Original comparison operation, or ALWAYS when comparison is disabled | uint32 |
| 24 | Original VkSamplerReductionMode | uint32 |
| 28 | Reserved, zero | uint32 |

Record flags 0x1 and 0x2 select linear mipmapping and enabled anisotropy. Original floating-point values retain their full precision. Native physical sampler parameters and physical border/reduction metadata have their own shared Sparse layout. The logical parameter address remains stable when native handles are shared. Sampled descriptors remain 64 bytes, and the native sampler table retains its 4096-handle prefix.

The common kk_sampler_query_metadata accessor uses selector6, KK_SAMPLER_META_MAX_ANISOTROPY, for a float32 dimensionless ratio: exactly1 when anisotropy is disabled and the original requested maximum when enabled. Selector7, KK_SAMPLER_META_REDUCTION_MODE, returns the original Vulkan reduction enum as an exact float32 value:0 for weighted average,1 for minimum and2 for maximum. Both values come from the logical parameter record for native and virtual sampling.

Existing native LOD queries preserve half-float rounding and the distinction between shader-lowered bias and hardware sampler bias. Virtual LOD values retain their original float32 precision. Pointer presence selects parameter decoding; it does not select virtual filtering.
