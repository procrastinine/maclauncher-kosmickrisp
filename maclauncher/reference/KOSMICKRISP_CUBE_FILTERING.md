# KosmicKrisp cube filtering

`VK_EXT_non_seamless_cube_map` provides the per-sampler
`VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT` flag. Without the flag,
cube sampling uses seamless face-edge filtering. With the flag, sampling
stays within the selected face and applies the sampler's address modes:
clamp to edge repeats edge texels, while clamp to border includes the
configured border value.

Cube and cube-array images use face-local Metal 2D-array views of the original
image. Coordinate projection and transformed gradients retain explicit mip
levels, comparison sampling, gather and anisotropic footprints. Views retain
their format, swizzle, array range and mip range. One image can be sampled
with either sampler state, and descriptor updates can change that state
without rebuilding the pipeline. The views use the public Metal API.

Implicit sampling retains its native cube sample for derivatives. A flagged
implicit operation also evaluates the face-local sample with projected
gradients. Non-cube operations use their ordinary texture lowering.

The flag occupies bit 2 of the existing sampler-filter metadata and adds no
native sampler-state entry. The face-view resource ID uses
`aux_resource_id` at byte 48 of the shared
[sampled-image descriptor](kosmickrisp-descriptor-layout.md). Cube writes
preserve `aux_word` at byte 56; the descriptor remains 64 bytes.

Cube and cube-array sampling supports descriptor sets, resource heaps and embedded
samplers. Each binding path uses the same sampler flag and face-local views;
heap texture dereferences retain their cube type when selecting the face view.
