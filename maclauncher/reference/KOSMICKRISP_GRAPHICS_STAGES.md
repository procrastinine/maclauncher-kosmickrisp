# KosmicKrisp graphics shader stages

## Subgroup operations

KosmicKrisp supports subgroup operations in vertex, tessellation control, tessellation evaluation, geometry, fragment and compute shaders. The subgroup size is 32. An execution group can have fewer active invocations, including the three active invocations of a three-control-point patch.

Native Metal vertex shaders use SIMD ballot, vote, shuffle, reduction and scan operations. Metal does not accept subgroup lane and width input attributes on vertex functions. The compiler derives the physical lane from the actual active mask and prefix rank at shader entry. Lane numbers remain stable through divergent control flow. Shaders without subgroup system values receive no lane-recovery code.

Geometry and tessellation use the existing Poly compute execution paths. Vertex shaders lowered through those paths use their compute subgroup inputs. Quad-operation stage restrictions and the required-subgroup-size stage mask retain their existing values.

## Geometry and tessellation multiview

`multiviewGeometryShader` and `multiviewTessellationShader` use per-view replay through the existing Poly draw path. Each active view executes its vertex, tessellation and geometry work with the corresponding `ViewIndex`. The final preraster stage writes the render-target layer. Tessellation evaluation is final when no geometry shader follows it; geometry is the final stage when present.

Vertex-only pipelines retain native vertex amplification. Poly multiview draws use one raster amplification per replay. The per-view index occupies an optional tail in the per-draw upload; ordinary draws retain their previous upload sizes and launch count. Graphics cache keys include a nonzero view mask even when the pipeline has no fragment shader.

Pipeline statistics count actual replayed work. Multiview query results and availability follow Vulkan's consecutive-query and summed-result rules. Transform feedback remains available for ordinary draws; Vulkan prohibits active transform feedback inside a multiview render pass. Mesh multiview is not exposed by this adaptation.

The [subgroup module](../../src/kosmickrisp/) and [Poly multiview module](../../src/kosmickrisp/) use the pinned Mesa source and the existing compiler, draw, descriptor and query paths.
