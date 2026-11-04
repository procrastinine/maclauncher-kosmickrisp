# MacLauncher adaptations

The driver is based on Mesa `4f071f891d63396c799b267172c19ba6b0301682`. Each adaptation is an ordinary commit. The implementation descriptions below identify the Vulkan contract, the Metal or compiler route and the consuming API. Feature availability also depends on the physical device queries and the selected Metal device. The two CPU builds use the same implementation; x86_64 code on Apple Silicon uses Rosetta. Intel GPUs are outside the Metal 4 device profile.

DXVK translates Direct3D 8 through 11, and vkd3d-proton translates Direct3D 12. ANGLE, Android and AnyPS5 consume the same Vulkan interfaces when their shaders or resource operations require them. A consumer association describes the API requirement, without asserting that every application uses that feature.

## Rasterization, geometry and queries

### Subgroup operations in graphics preraster stages

Vulkan contract: Subgroup operations in graphics preraster stages.

Native vertex lane indices come from actual SIMD active masks and ranks instead of unsupported Metal vertex input attributes. Poly stages use their existing compute SIMD execution.

Limits and reported properties: `subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT`.

Consumers: vkd3d-proton, DXVK, Vulkan graphics subgroup consumers.

Implementation: [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Poly transform feedback and generated primitive queries

Vulkan contract: `VK_EXT_transform_feedback`, `VK_EXT_primitives_generated_query`, `VkPhysicalDeviceTransformFeedbackFeaturesEXT.transformFeedback`, `VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT.primitivesGeneratedQuery`, `VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT.primitivesGeneratedQueryWithRasterizerDiscard`.

Retains upstream count/pre-GS passes, inserts VS/TES passthrough GS for XFB, wires counters and byte-count draws, and adds generated/written query types. Existing geometry and query paths change.

Scope: GS compilation/linking, shader serialization and lifetime, flat varying/provoking output selection, geometry parameter upload, query pool addressing and occlusion routing. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_query_pool.c](../src/kosmickrisp/vulkan/kk_query_pool.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_geometry.cl](../src/kosmickrisp/libkk/kk_geometry.cl).

### Native Metal line polygon state

Vulkan contract: Native Metal line polygon state.

Sets real Metal triangle fill state and restores FILL on transitions; POINT must also exist before fillModeNonSolid is advertised.

Scope: raster state flush and FILL/LINE transitions. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, Wine vkd3d, vkd3d-proton.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [mtl_encoder.h](../src/kosmickrisp/bridge/mtl_encoder.h), [mtl_encoder.m](../src/kosmickrisp/bridge/mtl_encoder.m), [mtl_encoder.c](../src/kosmickrisp/bridge/stubs/mtl_encoder.c).

### Capture, clip and rasterize triangle polygons as points

Vulkan contract: `VkPhysicalDeviceFeatures.fillModeNonSolid`, `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3PolygonMode`.

Adds a capture branch around original GS execution and XFB, clips with original weights/provoking data, then emits native square triangles with facing/depth semantics. Extends Poly parameters from 79 to 82 words before stream counters extend to 90. Corrected polygon_grid writes 64,1,1 threadgroup dimensions (c180bf43); this correction is part of the newly owned polygon kernel, not a separate upstream edit.

Limits and reported properties: `VkPhysicalDeviceMaintenance5PropertiesKHR.polygonModePointSize = false`.

Consumers: DXVK, Wine vkd3d, vkd3d-proton.

Implementation: [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_polygon_nir.h](../src/kosmickrisp/vulkan/kk_polygon_nir.h), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [meson.build](../src/kosmickrisp/libkk/meson.build), [kk_polygon.h](../src/kosmickrisp/libkk/kk_polygon.h), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl), [msl_iomap.c](../src/kosmickrisp/compiler/msl_iomap.c), [geometry.h](../src/poly/geometry.h), [geometry_capture.h](../src/poly/geometry_capture.h), [poly_nir.h](../src/poly/nir/poly_nir.h), [poly_nir_lower_gs.c](../src/poly/nir/poly_nir_lower_gs.c), [poly_nir_capture_gs.h](../src/poly/nir/poly_nir_capture_gs.h).

### Select and lower geometry rasterization streams

Vulkan contract: `VkPhysicalDeviceTransformFeedbackFeaturesEXT.geometryStreams`, `VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT.primitivesGeneratedQueryWithNonZeroStreams`.

Passes selected raster stream to Poly and suppresses nonzero single-stream/no-GS output without changing shader/XFB side effects.

Limits and reported properties: `VkPhysicalDeviceTransformFeedbackPropertiesEXT.transformFeedbackRasterizationStreamSelect = true`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Independent complete 64-bit generated and written reports

Vulkan contract: Independent complete 64-bit generated and written reports.

Uses 64-bit report pointers and a secondary generated destination so simultaneous query types do not overwrite one another. Parameter block grows 82 to 90 words.

Scope: Poly parameter layout and pre-GS report accumulation, simultaneous generated/XFB query destinations. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [geometry.h](../src/poly/geometry.h), [geometry.cl](../src/poly/cl/geometry.cl).

### Use unsigned wide arithmetic for transform feedback capacity

Vulkan contract: Use unsigned wide arithmetic for transform feedback capacity.

Avoids signed overflow around 2 GiB with uint64_t remaining bytes and division; preserves declared buffer range.

Scope: shared Poly XFB remaining-capacity calculation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [geometry.cl](../src/poly/cl/geometry.cl).

### Count generated primitives without a geometry shader

Vulkan contract: Count generated primitives without a geometry shader.

Adds one GPU counter pass only while the no-GS query is active; counts post-predicate/post-assembly direct, indirect, tessellated and restart-separated draws.

Scope: draw submission with active primitives query and no GS, restart-aware post-assembly count. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_geometry.cl](../src/kosmickrisp/libkk/kk_geometry.cl), [kk_draws.cl](../src/kosmickrisp/libkk/kk_draws.cl).

### All eleven core pipeline statistics

Vulkan contract: `pipelineStatisticsQuery`.

Packed 64-bit reports use actual input assembly, shader, Poly and dispatch producers while query destinations are active.

Scope: query reports, draw and dispatch accounting, fragment entry points, Poly geometry and tessellation. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton, Wine vkd3d, Vulkan query consumers.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_query_pool.c](../src/kosmickrisp/vulkan/kk_query_pool.c), [kk_cmd_dispatch.c](../src/kosmickrisp/vulkan/kk_cmd_dispatch.c), [kk_query.cl](../src/kosmickrisp/libkk/kk_query.cl), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_statistics_command.h](../src/kosmickrisp/vulkan/kk_statistics_command.h), [kk_statistics_nir.h](../src/kosmickrisp/vulkan/kk_statistics_nir.h), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Apply synchronization2 event dependencies through pipeline barriers

Vulkan contract: Apply synchronization2 event dependencies through pipeline barriers.

Applies every VkDependencyInfo through kk_CmdPipelineBarrier2, preserving shared attachment stores, encoder splits, compute barriers and event status. SYNC_FD and ordinary synchronization remain upstream.

Scope: synchronization2 event waits. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_event.c](../src/kosmickrisp/vulkan/kk_event.c), [kk_event_dependencies.h](../src/kosmickrisp/vulkan/kk_event_dependencies.h).

### Linear compute derivative groups

Vulkan contract: `VK_KHR_compute_shader_derivatives`, `computeDerivativeGroupLinear`, `computeDerivativeGroupQuads`.

Linear compute groups use four consecutive quad lanes; implicit sampling uses explicit gradients and LOD queries account for sampler bias, mipmap mode and available image levels. Sampled descriptors remain 64 bytes.

Limits and reported properties: `computeDerivativeGroupQuads = false`; `meshAndTaskShaderDerivatives = false`.

Consumers: vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_sampler.h](../src/kosmickrisp/vulkan/kk_sampler.h), [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [msl_compute_derivatives.h](../src/kosmickrisp/compiler/msl_compute_derivatives.h).

### Fragment helper semantics and inactive statistics storage

Vulkan contract: Fragment helper semantics and inactive statistics storage.

Fragment counters are compiled only for devices enabling pipelineStatisticsQuery, whose state participates in the graphics cache key. Instrumentation uses Metal helper-write suppression after application lowering. Null destinations disable Poly statistics writes, and inactive queries retain ordinary scratch sizes.

Scope: fragment helper and demote lowering, inactive tessellation and geometry scratch. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_statistics_nir.h](../src/kosmickrisp/vulkan/kk_statistics_nir.h), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [geometry.cl](../src/poly/cl/geometry.cl).

### Preserve full vertex-fragment interfaces through native varying packing

Vulkan contract: Preserve full vertex-fragment interfaces through native varying packing.

Interfaces exceeding 124 native varying components, including raster system inputs, share the per-corner heap transport and native basis interpolants. Smaller interfaces retain ordinary shaders, uploads and draws; the exposed 128-component limits are preserved.

Limits and reported properties: `maxVertexOutputComponents = 128`; `maxFragmentInputComponents = 128`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c).

### Fragment shader barycentric coordinates and per-vertex inputs

Vulkan contract: `VK_KHR_fragment_shader_barycentric`, `VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR.fragmentShaderBarycentric`.

Uses native center barycentrics and shared primitive-corner transport for original per-vertex inputs and off-center interpolation, including geometry adjacency and provoking vertex semantics.

Limits and reported properties: `triStripVertexOrderIndependentOfProvokingVertex = false`.

Consumers: vkd3d-proton.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_draws.cl](../src/kosmickrisp/libkk/kk_draws.cl).

### Preserve exact indirect tessellation dispatch dimensions

Vulkan contract: Preserve exact indirect tessellation dispatch dimensions.

Uses exact indirect thread grids with explicit local sizes for software VS, TCS and tessellator dispatch. Each TCS workgroup owns one patch and each VS invocation owns one input vertex, including partial workgroups and multiple instances.

Scope: indirect tessellation, generated tessellation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_tessellation.cl](../src/kosmickrisp/libkk/kk_tessellation.cl), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_tess_grid.h](../src/kosmickrisp/libkk/kk_tess_grid.h).

### Independent-stage primitive-corner transport

Vulkan contract: Independent-stage primitive-corner transport.

Independent producer and fragment stages share fixed native address/basis locations and location-based primitive-corner storage; linked and LTO stages retain compact interfaces.

Scope: independent non-LTO graphics-stage compilation. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton, DXVK.

Implementation: [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c), [kk_nir_lower_per_vertex.h](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.h), [kk_canonical_per_vertex.h](../src/kosmickrisp/vulkan/kk_canonical_per_vertex.h), [kk_barycentric_shader.h](../src/kosmickrisp/vulkan/kk_barycentric_shader.h).

### Scalar clip and cull varyings in generated polygon vertex shaders

Vulkan contract: Scalar clip and cull varyings in generated polygon vertex shaders.

Scalarizes captured clip/cull and cull-primitive output stores for Metal interface members before generated polygon vertex shader translation. Existing slot semantics and Poly clipping remain intact.

Scope: generated polygon rasterization vertex shader compilation. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, ANGLE GLES 3.2.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_polygon_scalar_io.h](../src/kosmickrisp/vulkan/kk_polygon_scalar_io.h).

### Conservative triangle rasterization through Poly expansion

Vulkan contract: `VK_EXT_conservative_rasterization`, `extendedDynamicState3ConservativeRasterizationMode`, `extendedDynamicState3ExtraPrimitiveOverestimationSize`.

Expands clipped triangle coverage to whole pixel spans and evaluates original-corner fragment attributes. Static disabled pipelines keep their original MSL and draw path.

Limits and reported properties: `primitiveOverestimationSize = 1.0f / 256.0f`; `degenerateTrianglesRasterized = true`; `fullyCoveredFragmentShaderInputVariable = true`; `conservativeRasterizationPostDepthCoverage = false`.

Consumers: vkd3d-proton, DXVK.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl), [kk_conservative.h](../src/kosmickrisp/libkk/kk_conservative.h), [kk_conservative.cl](../src/kosmickrisp/libkk/kk_conservative.cl), [kk_conservative_nir.h](../src/kosmickrisp/vulkan/kk_conservative_nir.h), [kk_nir_vertex_triplet.h](../src/kosmickrisp/vulkan/kk_nir_vertex_triplet.h), [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_polygon_nir.h](../src/kosmickrisp/vulkan/kk_polygon_nir.h), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_conservative_fs_link.h](../src/kosmickrisp/vulkan/kk_conservative_fs_link.h), [kk_conservative_fs_prolog.c](../src/kosmickrisp/vulkan/kk_conservative_fs_prolog.c), [meson.build](../src/kosmickrisp/vulkan/meson.build), [kk_compile_options.h](../src/kosmickrisp/vulkan/kk_compile_options.h).

### Multiview geometry and tessellation through real per-view Poly replay

Vulkan contract: `multiviewGeometryShader`, `multiviewTessellationShader`.

Native VS retains vertex amplification. Geometry and tessellation replay actual preraster/raster work for each view, with a per-draw view index and explicit layer output. Ordinary draws preserve upload size and launch count.

Scope: geometry and tessellation preraster launches, multiview layer selection, multiview pipeline query sums, view-mask shader cache keys without fragment stages, explicit per-view meta clears. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton ViewInstancing, Vulkan multiview GS/TES consumers.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_multiview_nir.h](../src/kosmickrisp/vulkan/kk_multiview_nir.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_cmd_meta.c](../src/kosmickrisp/vulkan/kk_cmd_meta.c).

### Task and mesh shaders with bounded task-grid virtualization

Vulkan contract: `VK_EXT_mesh_shader`, `taskShader`, `meshShader`, `meshShaderQueries`.

Metal mesh pipelines share graphics descriptors and fragment inputs. Bounded native object shaders and a batched compute task pre-pass share one shader body; prefix scans preserve payload ownership and virtual workgroup IDs.

Limits and reported properties: `maxTaskWorkGroupTotalCount = 4194304`; `maxMeshWorkGroupTotalCount = 4194304`; `maxTaskPayloadSize = 16384`; `maxMeshOutputVertices = 256`; `maxMeshOutputPrimitives = 512`.

Consumers: vkd3d-proton.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_query_pool.c](../src/kosmickrisp/vulkan/kk_query_pool.c), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [msl_private.h](../src/kosmickrisp/compiler/msl_private.h), [msl_iomap.c](../src/kosmickrisp/compiler/msl_iomap.c), [msl_type_inference.c](../src/kosmickrisp/compiler/msl_type_inference.c), [meson.build](../src/kosmickrisp/bridge/meson.build), [mtl_compiler.h](../src/kosmickrisp/bridge/mtl_compiler.h), [mtl_encoder.h](../src/kosmickrisp/bridge/mtl_encoder.h), [mtl_compiler.c](../src/kosmickrisp/bridge/stubs/mtl_compiler.c), [mtl_encoder.c](../src/kosmickrisp/bridge/stubs/mtl_encoder.c), [meson.build](../src/kosmickrisp/libkk/meson.build), [kk_mesh_types.h](../src/kosmickrisp/libkk/kk_mesh_types.h), [kk_mesh.cl](../src/kosmickrisp/libkk/kk_mesh.cl), [kk_mesh_kernels.h](../src/kosmickrisp/libkk/kk_mesh_kernels.h), [kk_mesh_nir.h](../src/kosmickrisp/vulkan/kk_mesh_nir.h), [kk_mesh_pipeline.h](../src/kosmickrisp/vulkan/kk_mesh_pipeline.h), [kk_mesh_command.h](../src/kosmickrisp/vulkan/kk_mesh_command.h), [kk_mesh_iomap.h](../src/kosmickrisp/compiler/kk_mesh_iomap.h), [kk_mesh_intrinsics.h](../src/kosmickrisp/compiler/kk_mesh_intrinsics.h), [kk_mesh_msl.h](../src/kosmickrisp/compiler/kk_mesh_msl.h), [kk_mesh_runtime.h](../src/kosmickrisp/compiler/kk_mesh_runtime.h), [mtl_mesh.m](../src/kosmickrisp/bridge/mtl_mesh.m), [kk_cmd_meta.c](../src/kosmickrisp/vulkan/kk_cmd_meta.c), [kk_vertex_input.h](../src/kosmickrisp/vulkan/kk_vertex_input.h), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [kk_mesh_pipeline_api.h](../src/kosmickrisp/vulkan/kk_mesh_pipeline_api.h), [kk_mesh_dispatch.h](../src/kosmickrisp/libkk/kk_mesh_dispatch.h), [kk_mesh_sliced.h](../src/kosmickrisp/vulkan/kk_mesh_sliced.h), [kk_canonical_layout.h](../src/kosmickrisp/libkk/kk_canonical_layout.h), [kk_mesh_canonical.h](../src/kosmickrisp/compiler/kk_mesh_canonical.h), [kk_canonical_per_vertex.h](../src/kosmickrisp/vulkan/kk_canonical_per_vertex.h), [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c), [kk_nir_lower_per_vertex.h](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.h).

### Wide, smooth and stippled lines through shared Poly capture

Vulkan contract: `VK_KHR_line_rasterization`, `VK_EXT_line_rasterization`, `wideLines`, `rectangularLines`, `smoothLines`, `stippledRectangularLines`, `stippledBresenhamLines`, `stippledSmoothLines`, `bresenhamLines`.

Shared Poly capture and clipping expand rectangular and smooth lines, reproduce Bresenham coverage, and carry per-strip stipple phase through the existing prefix scan and restart unroller. Smooth coverage modifies alpha before blending. Pure fragment shaders use an initial stipple discard; observable forced-early shaders retain early tests and receive only ON spans from Poly. Ordinary unit-width native draws retain their existing GPU path.

Limits and reported properties: `stippledRectangularLines = false`; `stippledSmoothLines = false`; `lineWidthRange = {1.0f, 4096.0f}`; `lineWidthGranularity = 1.0f / 256.0f`; `strictLines = false`; `nonStrictWideLinesUseParallelogram = true`.

Consumers: ANGLE, DXVK.

Implementation: [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [geometry_capture.h](../src/poly/geometry_capture.h), [kk_polygon.h](../src/kosmickrisp/libkk/kk_polygon.h), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl), [kk_polygon_nir.h](../src/kosmickrisp/vulkan/kk_polygon_nir.h), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [poly_nir_capture_gs.h](../src/poly/nir/poly_nir_capture_gs.h), [msl_iomap.c](../src/kosmickrisp/compiler/msl_iomap.c), [kk_draws.cl](../src/kosmickrisp/libkk/kk_draws.cl), [msl_nir_lower_common.c](../src/kosmickrisp/compiler/msl_nir_lower_common.c), [kk_lines_nir.h](../src/kosmickrisp/vulkan/kk_lines_nir.h), [restart.h](../src/poly/cl/restart.h), [kk_line_dashes.h](../src/kosmickrisp/libkk/kk_line_dashes.h).

### Preserve complete multi-draw commands during per-view replay

Vulkan contract: Preserve complete multi-draw commands during per-view replay.

Copy every direct multi-draw record into a temporary per-view snapshot before predicate and topology lowering. Indirect commands retain their fixed-size header and GPU record storage.

Scope: Poly multiview multi-draw replay. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan multiview applications.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c).

### Whole primitive CullDistance rejection

Vulkan contract: `shaderCullDistance`.

Preserve distinct clip/cull IO through translation and linking, retain native per-vertex addressing, and reject assembled primitives before rasterization. Poly dispatches use bounded threadgroups; mesh output uses Metal primitive_culled.

Scope: Poly primitive capture and clipping, Native mesh primitive output. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan graphics pipelines.

Implementation: [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [geometry_capture.h](../src/poly/geometry_capture.h), [kk_polygon.h](../src/kosmickrisp/libkk/kk_polygon.h), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl), [kk_polygon_nir.h](../src/kosmickrisp/vulkan/kk_polygon_nir.h), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [poly_nir_capture_gs.h](../src/poly/nir/poly_nir_capture_gs.h), [nir_to_msl.h](../src/kosmickrisp/compiler/nir_to_msl.h), [msl_nir_lower_common.c](../src/kosmickrisp/compiler/msl_nir_lower_common.c), [kk_conservative.cl](../src/kosmickrisp/libkk/kk_conservative.cl), [kk_mesh_iomap.h](../src/kosmickrisp/compiler/kk_mesh_iomap.h), [nir_shader_compiler_options.h](../src/compiler/nir/nir_shader_compiler_options.h), [nir_opt_varyings.c](../src/compiler/nir/nir_opt_varyings.c), [kk_cull_distance_indirect.h](../src/kosmickrisp/compiler/kk_cull_distance_indirect.h), [nir_spirv.h](../src/compiler/spirv/nir_spirv.h), [vk_nir.c](../src/vulkan/runtime/vk_nir.c), [poly_nir_passthrough_gs.c](../src/poly/nir/poly_nir_passthrough_gs.c).

### Bound generated geometry raster vertex IDs

Vulkan contract: Bound generated geometry raster vertex IDs.

Checks the decoded output-vertex, primitive and instance against the generated geometry domain before running shader replay or transform feedback. Invalid virtual IDs produce clipped output without reading the stage-input buffer.

Scope: geometry, tessellation feeding geometry. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [poly_nir_lower_gs.c](../src/poly/nir/poly_nir_lower_gs.c), [poly_gs_raster_bounds.h](../src/poly/nir/poly_gs_raster_bounds.h).

### Read retained point outputs for layered raster replay

Vulkan contract: Read retained point outputs for layered raster replay.

Select a read-only raster shader over retained point packets and indirect counts, preserving original user stages and transform feedback once per original view.

Scope: Ordered Poly point capture, Layered cached raster reader. Device support and operation restrictions are defined by the linked implementation.

Consumers: Layered point-output geometry pipelines, DGC layered sequence routing.

Implementation: [kk_poly_retained_points_nir.h](../src/kosmickrisp/vulkan/kk_poly_retained_points_nir.h), [kk_polygon_nir.h](../src/kosmickrisp/vulkan/kk_polygon_nir.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c).

### Carry cached raster primitive class through pipeline creation

Vulkan contract: Carry cached raster primitive class through pipeline creation.

Preserve native points and Bresenham lines versus expanded triangles in raster descriptors, serialization and attachment-cache identity.

Scope: Native raster pipeline creation, Poly draw primitive selection, Attachment specialization cache. Device support and operation restrictions are defined by the linked implementation.

Consumers: Layer-writing Vulkan meta shaders, Vulkan Poly rasterization.

Implementation: [kk_poly_raster_topology.h](../src/kosmickrisp/vulkan/kk_poly_raster_topology.h), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [mtl_compiler.h](../src/kosmickrisp/bridge/mtl_compiler.h), [mtl_compiler.m](../src/kosmickrisp/bridge/mtl_compiler.m).

### Skip empty pipeline dependencies

Vulkan contract: Skip empty pipeline dependencies.

Leave the current encoder intact for an empty dependency with zero flags and no extension chain. All memory barriers, execution dependencies and ownership transfers retain their existing handling.

Scope: Command recording and submission. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan applications.

Implementation: [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

### Preserve original views during cached Poly Layer replay

Vulkan contract: Preserve original views during cached Poly Layer replay.

Capture each original view once, preserve sparse ViewIndex values, and rasterize its cached output into the corresponding physical attachment alias. Ordinary native alternatives retain vertex amplification.

Scope: Poly multiview producer capture, cached Layer raster replay, native vertex amplification. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan multiview attachment aliases.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_poly_layer_multiview_nir.h](../src/kosmickrisp/vulkan/kk_poly_layer_multiview_nir.h), [kk_poly_layer_replay_command.h](../src/kosmickrisp/vulkan/kk_poly_layer_replay_command.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

### Preserve ordinary compute pipeline bindings

Vulkan contract: Preserve ordinary compute pipeline bindings.

Omit repeated native pipeline binds for ordinary dispatches. New compute encoders, shader binds and internal compute acquisition invalidate the binding cache; internal predication and statistics retain their pipeline restoration.

Scope: Compute dispatch encoding. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan compute applications.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_dispatch.c](../src/kosmickrisp/vulkan/kk_cmd_dispatch.c).

### Export prepared Poly raster batches

Vulkan contract: Export prepared Poly raster batches.

Export a prepared raster-only batch and select original primitive ranges from GPU prefixes without repeating original producer stages.

Scope: Poly ordered capture and prefix, Cached raster readers. Device support and operation restrictions are defined by the linked implementation.

Consumers: Fragment shading rate retained raster phases.

Implementation: [kk_poly_retained_raster_types.h](../src/kosmickrisp/vulkan/kk_poly_retained_raster_types.h), [kk_poly_retained_raster_command.h](../src/kosmickrisp/vulkan/kk_poly_retained_raster_command.h), [kk_poly_retained.h](../src/kosmickrisp/libkk/kk_poly_retained.h), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_poly_layer_replay_nir.h](../src/kosmickrisp/vulkan/kk_poly_layer_replay_nir.h), [kk_poly_layer_replay_command.h](../src/kosmickrisp/vulkan/kk_poly_layer_replay_command.h).

### Share linked vertex input slots with native raster alternatives

Vulkan contract: Share linked vertex input slots with native raster alternatives.

Lower native alternative vertex fetches after linking using the owning shader input mask. Static formats, dynamic metadata and robustness bounds use the same compact root slots as the ordinary draw state.

Scope: Linked vertex input packing, Native raster alternative compilation. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan graphics pipelines with native raster alternatives.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

## Resources, descriptors and memory

### Shared compiler-assigned shader feature bits for pipeline statistics and sampler reduction

Vulkan contract: Shared compiler-assigned shader feature bits for pipeline statistics and sampler reduction.

Assigns every shader feature a unique compiler-selected bit index in the shared shader header, checks mask capacity and includes the feature count in cache-key state.

Scope: shader feature masks, shader-cache state hashing. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

### Reject invalid heap and host imports before Metal access

Vulkan contract: `VK_EXT_external_memory_host`, `VK_EXT_external_memory_metal`.

Checks handle kind, null values, host alignment and native allocation before size, address and residency operations; releases retained heap/wrapper on failure.

Scope: host-pointer imports, Metal-heap imports. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c).

### Clamp attachmentless Metal dimensions to one

Vulkan contract: Clamp attachmentless Metal dimensions to one.

Uses MAX2(extent, 1) for Metal framebuffer width/height while preserving the Vulkan area.

Scope: attachmentless rendering descriptor creation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c).

### Preserve unaffected custom-border sample components

Vulkan contract: Preserve unaffected custom-border sample components.

Preserves bitwise-identical zero-border and one-border samples before custom-border interpolation, including non-finite interior texels.

Scope: filtered custom-border texture lowering. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton.

Implementation: [kk_nir_lower_custom_border.c](../src/kosmickrisp/vulkan/kk_nir_lower_custom_border.c).

### Change sample count between attachmentless draws

Vulkan contract: `VkPhysicalDeviceFeatures.variableMultisampleRate`.

Tracks attachmentless scopes, updates default raster sample count when pipeline samples change, and restarts the encoder.

Scope: attachmentless render state, native encoder restart. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Zero-initialized device memory and image layouts

Vulkan contract: `VK_EXT_zero_initialize_device_memory`, `zeroInitializeDeviceMemory`.

Exposes zero-initialized allocation requests and preserving ZERO_INITIALIZED image layouts. The zero-initialize-allocation-clear correction establishes shared and private allocation contents explicitly. ZERO_INITIALIZED images use unoptimized Metal storage to preserve zero bits without Apple lossless texture compression; other initial layouts use their normal storage selection.

Scope: shared image layout initialization. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image_layout.c](../src/kosmickrisp/vulkan/kk_image_layout.c), [kk_image_layout.h](../src/kosmickrisp/vulkan/kk_image_layout.h), [kk_zero_init.h](../src/kosmickrisp/vulkan/kk_zero_init.h).

### Private device memory

Vulkan contract: Private device memory.

Device-local allocations use private Metal placement heaps through the common allocator and binding path. Internal uploads and host-visible allocations retain shared storage.

Limits and reported properties: `memoryTypes = ["DEVICE_LOCAL", "DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED"]`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_bo.c](../src/kosmickrisp/vulkan/kk_bo.c), [kk_bo.h](../src/kosmickrisp/vulkan/kk_bo.h), [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [kk_buffer.c](../src/kosmickrisp/vulkan/kk_buffer.c), [mtl_heap.m](../src/kosmickrisp/bridge/mtl_heap.m), [mtl_heap.h](../src/kosmickrisp/bridge/mtl_heap.h).

### Unused dynamic-rendering attachments

Vulkan contract: `VK_EXT_dynamic_rendering_unused_attachments`, `VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT.dynamicRenderingUnusedAttachments`.

Cached Metal format variants share shader functions and pre-raster programs; unused color writes are discarded and depth/stencil state follows the attachments used by both pipeline and pass.

Scope: render pipeline compilation and binding, fragment color output lowering, depth/stencil state and shader lifetime. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [mtl_compiler.h](../src/kosmickrisp/bridge/mtl_compiler.h), [mtl_compiler.m](../src/kosmickrisp/bridge/mtl_compiler.m), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Shader sampler minimum and maximum reduction

Vulkan contract: `VK_EXT_sampler_filter_minmax`, `VkPhysicalDeviceVulkan12Features.samplerFilterMinmax`.

Uses descriptor padding for minimum/maximum footprint reduction on GPUs without native sampler reduction; native reduction and feature-disabled shaders retain their existing path.

Limits and reported properties: `filterMinmaxSingleComponentFormats = supported_extensions->EXT_sampler_filter_minmax`; `filterMinmaxImageComponentMapping = supported_extensions->EXT_sampler_filter_minmax`.

Consumers: vkd3d-proton.

Implementation: [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h), [kk_sampler.h](../src/kosmickrisp/vulkan/kk_sampler.h), [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_sampler_minmax.h](../src/kosmickrisp/vulkan/kk_sampler_minmax.h), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c).

### Fractional image-view minimum LOD

Vulkan contract: `VK_EXT_image_view_min_lod`, `minLod`.

Enables the existing Mesa descriptor and NIR lowering for fractional image-view minimum LOD.

Scope: sampled-image shader operations when minLod is enabled. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton ResourceMinLODClamp.

Implementation: [kk_debug.c](../src/kosmickrisp/vulkan/kk_debug.c), [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h).

### Sliced 3D storage image views

Vulkan contract: `VK_EXT_image_sliced_view_of_3d`, `imageSlicedViewOf3D`.

Shares existing descriptor metadata words for the storage slice range and translates Z coordinates and size queries in NIR. Sampled descriptors retain the full mip depth.

Scope: 3D storage-image descriptor writes, 3D storage-image shader operations. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton 3D UAV FirstWSlice/WSize.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_sliced_3d.h](../src/kosmickrisp/vulkan/kk_sliced_3d.h).

### Extended push constants and descriptors with layout-sized uploads

Vulkan contract: Extended push constants and descriptors with layout-sized uploads.

Supports 4096-byte constants across graphics and compute and 256 push descriptors. Root uploads cover fixed state plus the bound layout range; descriptor storage and uploads use the actual layout size. Partial and template updates retain untouched values.

Limits and reported properties: `VkPhysicalDeviceLimits.maxPushConstantsSize = KK_MAX_PUSH_SIZE`; `VkPhysicalDeviceVulkan14Properties.maxPushDescriptors = KK_MAX_PUSH_DESCRIPTORS`; `VkPhysicalDevicePushDescriptorPropertiesKHR.maxPushDescriptors = KK_MAX_PUSH_DESCRIPTORS`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, Native Vulkan.

Implementation: [kk_private.h](../src/kosmickrisp/vulkan/kk_private.h), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_descriptor_set.h](../src/kosmickrisp/vulkan/kk_descriptor_set.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c).

### Apple float controls and resource limits

Vulkan contract: `VK_KHR_shader_float_controls`.

Reports fixed Apple FP16 preserve and FP32 flush modes, canonicalizes explicit FP32 flush results including fcanonicalize, and supports 64 uniform plus 64 storage dynamic descriptors and 8192 allocations. The existing push-data uploader retains the enlarged fixed descriptor prefix and the bound pipeline constant tail.

Limits and reported properties: `VkPhysicalDeviceVulkan12Properties.denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL`; `VkPhysicalDeviceVulkan12Properties.roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL`; `VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat16 = true`; `VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat32 = true`; `VkPhysicalDeviceLimits.maxMemoryAllocationCount = 8192`; `VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceMaintenance7PropertiesKHR.maxDescriptorSetTotalUniformBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceMaintenance7PropertiesKHR.maxDescriptorSetTotalStorageBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceMaintenance7PropertiesKHR.maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceMaintenance7PropertiesKHR.maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS / 2`; `VkPhysicalDeviceMaintenance7PropertiesKHR.maxDescriptorSetUpdateAfterBindTotalBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS`; `VkPhysicalDeviceMaintenance7PropertiesKHR.maxDescriptorSetTotalBuffersDynamic = KK_MAX_DYNAMIC_BUFFERS`.

Consumers: vkd3d-proton, DXVK, Wine vkd3d, Vulkan descriptor and allocation consumers.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kosmickrisp-float-controls.h](../src/kosmickrisp/compiler/kosmickrisp-float-controls.h), [kk_private.h](../src/kosmickrisp/vulkan/kk_private.h), [kk_descriptor_set_layout.c](../src/kosmickrisp/vulkan/kk_descriptor_set_layout.c).

### 32-bit integer multisample images and sample-zero resolves

Vulkan contract: 32-bit integer multisample images and sample-zero resolves.

Queries Metal 32-bit MSAA capability and uses the shared sample-zero meta resolve for integer color images.

Scope: format properties, shared descriptor and Metal representation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_physical_device.h](../src/kosmickrisp/vulkan/kk_physical_device.h), [mtl_device.h](../src/kosmickrisp/bridge/mtl_device.h), [mtl_device.m](../src/kosmickrisp/bridge/mtl_device.m).

### Alpha, packed 16-bit and swizzled BGRA sampled images

Vulkan contract: Alpha, packed 16-bit and swizzled BGRA sampled images.

Adds A8 and packed 16-bit image formats and sampled-only BGRA SNORM/UINT/SINT views. Fixed opaque black uses the shared sampler correction. A8 color attachments remain unavailable.

Scope: format properties, shared descriptor and Metal representation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

### Rasterization-ordered color attachment reads

Vulkan contract: `VK_EXT_rasterization_order_attachment_access`, `VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT.rasterizationOrderColorAttachmentAccess`, `VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT.rasterizationOrderDepthAttachmentAccess`, `VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT.rasterizationOrderStencilAttachmentAccess`.

Shares native programmable-blending color reads across scalar, multisample and dynamically indexed input attachments. Keeps custom-resolve sources, other samples, unmapped inputs and depth/stencil reads on texture descriptors. Native color reads provide the pipeline/subpass attachment-order guarantee without per-draw work.

Limits and reported properties: `VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT.rasterizationOrderDepthAttachmentAccess = false`; `VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT.rasterizationOrderStencilAttachmentAccess = false`.

Consumers: ANGLE GLES 3.2, Native Vulkan.

Implementation: [msl_nir_lower_input_attachments.c](../src/kosmickrisp/compiler/msl_nir_lower_input_attachments.c), [msl_attachment_samples.h](../src/kosmickrisp/compiler/msl_attachment_samples.h), [nir_to_msl.h](../src/kosmickrisp/compiler/nir_to_msl.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_attachment_device.h](../src/kosmickrisp/vulkan/kk_attachment_device.h).

### Dynamic vertex layouts with shared format decoding

Vulkan contract: `VK_EXT_vertex_input_dynamic_state`, `vertexInputDynamicState`.

Decodes draw-time format, binding, stride and divisor metadata through the same lowering used by static layouts. Dynamic metadata uploads are sized to shader inputs.

Scope: vertex fetch, vertex input state, shader keys. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, ANGLE.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_nir_lower_vbo.c](../src/kosmickrisp/vulkan/kk_nir_lower_vbo.c), [kk_nir_lower_vbo.h](../src/kosmickrisp/vulkan/kk_nir_lower_vbo.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_vertex_fetch.h](../src/kosmickrisp/vulkan/kk_vertex_fetch.h), [kk_vertex_input.h](../src/kosmickrisp/vulkan/kk_vertex_input.h).

### Buffer and shared 64-bit integer atomics

Vulkan contract: `VK_KHR_shader_atomic_int64`, `shaderBufferInt64Atomics`, `shaderSharedInt64Atomics`.

Hashed locks serialize full-width operations over 32-bit atomic halves. Retry-loop results use a 32-bit pair before reconstruction. Unused native unsigned min/max retain the same lock.

Scope: root descriptor upload, device allocation, atomic emission. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_atomic_int64.h](../src/kosmickrisp/compiler/kk_atomic_int64.h), [kk_atomic_int64_msl.h](../src/kosmickrisp/compiler/kk_atomic_int64_msl.h).

### Fixed auxiliary words in sampled-image descriptors

Vulkan contract: Fixed auxiliary words in sampled-image descriptors.

Names the auxiliary resource ID and auxiliary word at byte offsets 48 and 56 and retains the 64-byte sampled-image descriptor layout.

Scope: sampled-image descriptor layout. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan sampled-image descriptor consumers.

Implementation: [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h).

### Sample-scoped fragment interlock for storage resources

Vulkan contract: `VK_EXT_fragment_shader_interlock`, `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderSampleInterlock`, `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderPixelInterlock`, `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderShadingRateInterlock`.

An explicit native raster-order guard brackets storage accesses with the SPIR-V device and texture fences, including physical buffer addresses. Existing descriptor layout and lowering remain shared. Discard releases an active region before helper writes are suppressed.

Limits and reported properties: `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderPixelInterlock = gpu_apple_family >= 7`; `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderShadingRateInterlock = false`.

Consumers: Native Vulkan.

Implementation: [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [msl_interlock_guard.h](../src/kosmickrisp/compiler/msl_interlock_guard.h), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_interlock_device.h](../src/kosmickrisp/vulkan/kk_interlock_device.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Dynamic raster samples, sample mask and alpha-to-coverage

Vulkan contract: `VK_EXT_extended_dynamic_state3`, `extendedDynamicState3RasterizationSamples`, `extendedDynamicState3SampleMask`, `extendedDynamicState3AlphaToCoverageEnable`.

Selects cached native sample-count variants and supplies draw-time sample mask and alpha-to-coverage through the shared fragment lowering. Static and dynamic state share the same implementations.

Scope: multisample state, attachment variants, attachmentless encoder restart, fragment sample masks, alpha-to-coverage. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, ANGLE.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [kk_dynamic_multisample.h](../src/kosmickrisp/vulkan/kk_dynamic_multisample.h), [nir_to_msl.h](../src/kosmickrisp/compiler/nir_to_msl.h), [msl_nir_lower_common.c](../src/kosmickrisp/compiler/msl_nir_lower_common.c), [mtl_compiler.h](../src/kosmickrisp/bridge/mtl_compiler.h), [mtl_compiler.m](../src/kosmickrisp/bridge/mtl_compiler.m).

### Implicit multisample attachments for single-sampled rendering

Vulkan contract: `VK_EXT_multisampled_render_to_single_sampled`, `multisampledRenderToSingleSampled`.

Replicates loaded values to command-owned multisample images and resolves into original single-sampled attachments through shared attachment paths.

Scope: rendering attachments, meta blits, attachment resolves. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE, DXVK, vkd3d-proton.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_meta.c](../src/kosmickrisp/vulkan/kk_cmd_meta.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [vk_meta_blit_resolve.c](../src/vulkan/runtime/vk_meta_blit_resolve.c), [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [kk_msrtss.h](../src/kosmickrisp/vulkan/kk_msrtss.h).

### Demoted fragment memory effects

Vulkan contract: Demoted fragment memory effects.

After API-specific NIR lowering and dead-code cleanup, fragment demote/terminate shaders that write memory use shared Mesa write predication. Demoting shaders with helper queries also track the initial and demoted helper state even without memory writes. Skipped atomic results are zero and termination keeps its return. Other shaders retain their emitted Metal source.

Scope: fragment shaders with demote or terminate and external memory writes. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, ANGLE GLES 3.2.

Implementation: [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_helper_side_effects.h](../src/kosmickrisp/compiler/kk_helper_side_effects.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

### Preserve remapped dynamic-rendering color outputs

Vulkan contract: Preserve remapped dynamic-rendering color outputs.

Maps only declared physical color attachment formats into blend options. Unused identity entries cannot overwrite a live remapped output; unused output stores continue to be discarded.

Scope: fragment blend and unused-output lowering. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

### RGB32 image backing and exact transfer conversion

Vulkan contract: RGB32 image backing and exact transfer conversion.

RGB32 optimal images use RGBA32 backing with exact packed RGB copies, sampled/storage views, and RGB32 texel-buffer load/store lowering. Staging stays bounded to four MiB and only RGB operations invoke conversion.

Scope: shared image and descriptor handling. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_format.h](../src/kosmickrisp/vulkan/kk_format.h), [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image_layout.c](../src/kosmickrisp/vulkan/kk_image_layout.c), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_cmd_copy.c](../src/kosmickrisp/vulkan/kk_cmd_copy.c), [kk_buffer_view.c](../src/kosmickrisp/vulkan/kk_buffer_view.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_rgb32_copy.h](../src/kosmickrisp/vulkan/kk_rgb32_copy.h), [kk_query.cl](../src/kosmickrisp/libkk/kk_query.cl), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c).

### Alpha and packed 16-bit image operations

Vulkan contract: Alpha and packed 16-bit image operations.

A8 retains logical RGBA through blending and the Metal color interface. Native packed 16-bit layouts and their shared swizzled views support sampling, attachments, and unformatted storage reads and writes. Uniform and storage texel-buffer views use the same native format swizzle, including writes, without changing descriptors.

Scope: shared image and descriptor handling. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [mtl_buffer.m](../src/kosmickrisp/bridge/mtl_buffer.m), [mtl_texture.m](../src/kosmickrisp/bridge/mtl_texture.m).

### Dynamic multisample color input attachments

Vulkan contract: Dynamic multisample color input attachments.

Exposes the shared multisample input-attachment texture lowering with sample-preserving encoder splits at framebuffer dependencies.

Limits and reported properties: `dynamicRenderingLocalReadMultisampledAttachments = true`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Shared sampler emulation helpers

Vulkan contract: Shared sampler emulation helpers.

Shares sampler reduction and border implementations across matching sample signatures, preserving native sampling and immutable sampler specialization.

Scope: sampler reduction, custom border sampling. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Extended image create and usage flags

Vulkan contract: `VK_KHR_extended_flags`, `VkPhysicalDeviceExtendedFlagsFeaturesKHR.extendedFlags`.

Uses Mesa runtime pNext handling for 64-bit image flags and view usage, preserves the full flag width through Metal layout, and initializes the separate FormatProperties4 namespace.

Scope: image format queries, image creation and layout, format properties. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image_layout.c](../src/kosmickrisp/vulkan/kk_image_layout.c), [kk_image_layout.h](../src/kosmickrisp/vulkan/kk_image_layout.h), [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [vk_image.c](../src/vulkan/runtime/vk_image.c).

### Publish tessellation prefix totals across device memory

Vulkan contract: Publish tessellation prefix totals across device memory.

Includes device memory in the workgroup fence between prefix-sum writes and the elected lane reading the inclusive total to allocate indices and publish the generated draw.

Scope: tessellation, indirect tessellation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_geometry.cl](../src/kosmickrisp/libkk/kk_geometry.cl).

### Per-sampler non-seamless cube filtering

Vulkan contract: `VK_EXT_non_seamless_cube_map`, `nonSeamlessCubeMap`.

Sampler flags select face-local 2D-array filtering for cube operations. Ordinary 2D and 3D instructions retain their native lowering. Quotient-rule gradients preserve mip selection and anisotropic footprints.

Scope: shared descriptor auxiliary resource word, sampler filter metadata, texture view creation, texture lowering. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK D3D9.

Implementation: [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_image_view.h](../src/kosmickrisp/vulkan/kk_image_view.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_non_seamless_cube.h](../src/kosmickrisp/vulkan/kk_non_seamless_cube.h).

### Descriptor heaps and shader resource mappings

Vulkan contract: `VK_EXT_descriptor_heap`, `VkPhysicalDeviceDescriptorHeapFeaturesEXT.descriptorHeap`.

Sets and heaps share one sampled-image writer with fixed auxiliary words for multi-planar native IDs, preserving single-plane image metadata. Multi-planar formats exclude cube and sparse creation flags. Fixed heap records point to shared sampler metadata. Texel strides and writes use the shared type layout; Mesa lowers constant, push and indirect mappings. Native views follow parent resources, and native sampler IDs share the indexed-sampler cache. Heap-enabled devices enqueue Vulkan commands and reuse native replay at submission so descriptor writes and native resource creation completed after recording are visible. Devices without descriptor heaps retain their primary immediate path; drawable synchronization follows the native command buffer that is committed. Adjacent uses of the same retained pipeline layout share its command-queue reference while preserving every recorded command and final layout destruction. Ordinary push constants and heap push data share bounded byte-range updates and retain existing root dirtiness when bytes are unchanged. Ordinary upload and generated graphics or compute roots apply heap aliases to their uploaded or local snapshots through one helper, retaining CPU set bindings and one upload copy. Shared sampler emulation reads runtime metadata for heap casts. Packed sampler metadata and helper LOD sources retain explicit unsigned and floating-point MSL types, including constant embedded sampler addresses.

Scope: descriptor lowering, resource lifetime. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_buffer.c](../src/kosmickrisp/vulkan/kk_buffer.c), [kk_buffer.h](../src/kosmickrisp/vulkan/kk_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_descriptor_set.h](../src/kosmickrisp/vulkan/kk_descriptor_set.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image.h](../src/kosmickrisp/vulkan/kk_image.h), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_sampler.h](../src/kosmickrisp/vulkan/kk_sampler.h), [kk_sampler_minmax.h](../src/kosmickrisp/vulkan/kk_sampler_minmax.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [msl_type_inference.c](../src/kosmickrisp/compiler/msl_type_inference.c), [kk_descriptor_heap_api.h](../src/kosmickrisp/vulkan/kk_descriptor_heap_api.h), [kk_descriptor_heap_nir.h](../src/kosmickrisp/vulkan/kk_descriptor_heap_nir.h), [kk_dispatch_cmd_gen.py](../src/kosmickrisp/util/kk_dispatch_cmd_gen.py), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [vk_cmd_queue_gen.py](../src/vulkan/util/vk_cmd_queue_gen.py), [kk_cmd_dgc.c](../src/kosmickrisp/vulkan/kk_cmd_dgc.c), [kk-sampler-helpers-nir.h](../src/kosmickrisp/vulkan/kk-sampler-helpers-nir.h).

### Full-width signed and unsigned image atomics

Vulkan contract: `VK_EXT_shader_image_atomic_int64`, `VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT.shaderImageInt64Atomics`.

Uses exact RG32Uint backing for R64_UINT and R64_SINT images, with the shared full-width operation evaluator and CAS lock table for returning and signed atomics, and address-based lowering for buffer views.

Scope: storage images, storage texel buffers, 64-bit atomic lock table. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton AtomicInt64OnTypedResource, vkd3d-proton SamplerFeedbackTier 0.9, DXVK, ANGLE GLES 3.2.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_format.c](../src/kosmickrisp/vulkan/kk_format.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_buffer_view.c](../src/kosmickrisp/vulkan/kk_buffer_view.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_atomic_int64.h](../src/kosmickrisp/compiler/kk_atomic_int64.h), [kk_image_atomic_int64_msl.h](../src/kosmickrisp/compiler/kk_image_atomic_int64_msl.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Placement sparse buffer and image binding

Vulkan contract: `sparseBinding`, `sparseResidencyBuffer`, `sparseResidencyImage2D`, `sparseResidencyImage3D`, `sparseResidencyAliased`, `shaderResourceResidency`.

Maps 64 KiB private heap pages into sparse buffers and optimal textures, including per-layer mip tails, and emits native shader residency queries.

Limits and reported properties: `residencyNonResidentStrict = "GPU buffer residency guards and native strict texture access"`; `residencyStandard2DBlockShape = true`; `residencyStandard3DBlockShape = false`.

Consumers: DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_sparse.c](../src/kosmickrisp/vulkan/kk_sparse.c), [kk_buffer.c](../src/kosmickrisp/vulkan/kk_buffer.c), [kk_buffer_view.c](../src/kosmickrisp/vulkan/kk_buffer_view.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [mtl_heap.m](../src/kosmickrisp/bridge/mtl_heap.m).

### Timestamp pools spanning native counter heaps

Vulkan contract: Timestamp pools spanning native counter heaps.

Large pools use native heaps sized from actual accepted allocations. Vulkan reports remain linear; repeated render-stage samples retain their real heap and local index across chunk boundaries. Single-heap pools retain their original native allocation and index path.

Scope: timestamp pool allocation, timestamp render-stage reuse, timestamp resolve indexing. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan timestamp queries, vkd3d-proton.

Implementation: [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_query_pool.h](../src/kosmickrisp/vulkan/kk_query_pool.h), [kk_query_pool.c](../src/kosmickrisp/vulkan/kk_query_pool.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_timestamp_heaps.h](../src/kosmickrisp/vulkan/kk_timestamp_heaps.h), [mtl_device.h](../src/kosmickrisp/bridge/mtl_device.h), [mtl_device.m](../src/kosmickrisp/bridge/mtl_device.m), [mtl_device.c](../src/kosmickrisp/bridge/stubs/mtl_device.c).

### Memory priority and pageable residency

Vulkan contract: `VK_EXT_memory_priority`, `VK_EXT_pageable_device_local_memory`, `memoryPriority`, `pageableDeviceLocalMemory`.

Metal queues retain execution residency; allocations above the default priority additionally request idle residency. Pageable mode releases the whole-device idle residency request, preserving contents without purgeability.

Scope: memory allocation, residency sets, queue submission. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_device_memory.h](../src/kosmickrisp/vulkan/kk_device_memory.h), [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Image compression control

Vulkan contract: `VK_EXT_image_compression_control`, `imageCompressionControl`.

Explicit compression disable clears native GPU-optimized contents and reports disabled compression. Other controls retain Metal lossless/default behavior with no fixed-rate compression advertised.

Scope: image layout selection, native texture descriptors, image compression queries. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton.

Implementation: [kk_image_layout.c](../src/kosmickrisp/vulkan/kk_image_layout.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Native image alignment hints

Vulkan contract: `VK_MESA_image_alignment_control`, `imageAlignmentControl`.

Exposes best-effort 4/16/64 KiB image alignment hints over the existing minimum native-layout query, preserving compression. Native minimum alignment remains authoritative when a requested maximum cannot be met.

Limits and reported properties: `supportedImageAlignmentMask = 86016`.

Consumers: vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Shared immutable sampler parameters

Vulkan contract: Shared immutable sampler parameters.

Defines the immutable fp32 sampler record and generates matching host and Metal field and flag declarations. Native sampler descriptors retain their existing representation.

Scope: native sampler metadata. Device support and operation restrictions are defined by the linked implementation.

Consumers: sampler emulation, descriptor heaps.

Implementation: [kk-sampler-params.h](../src/kosmickrisp/compiler/kk-sampler-params.h), [kk-sampler-params-init.h](../src/kosmickrisp/vulkan/kk-sampler-params-init.h).

### Immutable sampler parameter arena

Vulkan contract: Immutable sampler parameter arena.

Sampler metadata and immutable original parameter records share device-retained resident pages. Constructor calls carry their heap-lock context explicitly, and one host helper packs full GPU addresses into sampler-owned metadata fields.

Scope: heap sampler metadata allocation, sampler construction. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_sampler.h](../src/kosmickrisp/vulkan/kk_sampler.h), [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_descriptor_heap_api.h](../src/kosmickrisp/vulkan/kk_descriptor_heap_api.h), [kk_sampler_params_api.h](../src/kosmickrisp/vulkan/kk_sampler_params_api.h), [kk_sampler_params_impl.h](../src/kosmickrisp/vulkan/kk_sampler_params_impl.h).

### Owning POSIX external memory handles

Vulkan contract: `VK_KHR_external_memory_fd`.

Adds owning OPAQUE_FD memory backed by shared no-copy buffers, including compatible single-layer two-dimensional external color images. Ordinary resources retain their existing allocation and encoding paths.

Scope: ordinary heap allocation, Metal and host imports, resource binding and encoding. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, winevulkan.

Implementation: [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [kk_device_memory.h](../src/kosmickrisp/vulkan/kk_device_memory.h), [kk_buffer.c](../src/kosmickrisp/vulkan/kk_buffer.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image_layout.c](../src/kosmickrisp/vulkan/kk_image_layout.c), [kk_external_memory_fd.h](../src/kosmickrisp/vulkan/kk_external_memory_fd.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [mtl_buffer.h](../src/kosmickrisp/bridge/mtl_buffer.h), [mtl_buffer.m](../src/kosmickrisp/bridge/mtl_buffer.m), [mtl_buffer.c](../src/kosmickrisp/bridge/stubs/mtl_buffer.c).

### Render continuations and transient attachment storage

Vulkan contract: Render continuations and transient attachment storage.

Retains render encoders through framebuffer-local dependencies and adjacent dynamic rendering continuations; transient lazy images use tile storage or committed private backing according to the complete command sequence.

Limits and reported properties: `memoryTypes = ["DEVICE_LOCAL | LAZILY_ALLOCATED"]`.

Consumers: ANGLE, DXVK, vkd3d-proton.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_whole_pass.h](../src/kosmickrisp/vulkan/kk_whole_pass.h), [kk_dispatch_cmd_gen.py](../src/kosmickrisp/util/kk_dispatch_cmd_gen.py), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [kk_transient.h](../src/kosmickrisp/vulkan/kk_transient.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [kk_device_memory.h](../src/kosmickrisp/vulkan/kk_device_memory.h), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image.h](../src/kosmickrisp/vulkan/kk_image.h), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_image_view.h](../src/kosmickrisp/vulkan/kk_image_view.h), [kk_buffer.c](../src/kosmickrisp/vulkan/kk_buffer.c), [mtl_heap.m](../src/kosmickrisp/bridge/mtl_heap.m), [mtl_heap.h](../src/kosmickrisp/bridge/mtl_heap.h).

### Single-layer image descriptor aliasing

Vulkan contract: `VK_KHR_maintenance11`, `maintenance11`.

Selects native plain and array image views through fixed-size descriptors and shared indirect sparse query records, retaining heap-safe access qualifiers and multisample representation ownership.

Scope: image view creation, sampled and storage descriptors, array image atomics. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton, DXVK.

Implementation: [kk_maintenance11.h](../src/kosmickrisp/vulkan/kk_maintenance11.h), [kk_image_view.h](../src/kosmickrisp/vulkan/kk_image_view.h), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Aligned ordinary logical heap bases

Vulkan contract: Aligned ordinary logical heap bases.

Ordinary owned placement-heap allocations of at least 64 KiB use a checked 64 KiB logical GPU base. Hidden heap prefix/capacity stays outside the logical region; retained heap imports preserve that region. Sparse-compatible private allocations publish zero physical prefix. Foreign and externally backed no-copy allocations preserve their existing payload origin.

Scope: ordinary BO allocation, memory import, buffer/image binding. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Vulkan buffer device addresses.

Implementation: [kk_bo.h](../src/kosmickrisp/vulkan/kk_bo.h), [kk_bo.c](../src/kosmickrisp/vulkan/kk_bo.c), [kk_device_memory.h](../src/kosmickrisp/vulkan/kk_device_memory.h), [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [kk_buffer.c](../src/kosmickrisp/vulkan/kk_buffer.c), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [mtl_heap.h](../src/kosmickrisp/bridge/mtl_heap.h), [mtl_heap.m](../src/kosmickrisp/bridge/mtl_heap.m).

### Shared BO residency for external FD allocations

Vulkan contract: Shared BO residency for external FD allocations.

Register FD-backed BOs through the common residency helper and finalize their allocation priority through the shared memory path.

Scope: External FD allocation and release. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, winevulkan.

Implementation: [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c).

### Per-sample storage backing for multisample color images

Vulkan contract: `shaderStorageImageMultisample`.

Uses immutable per-view sample-resource tables and existing memory bindings for multisample storage operations, with native attachment and input publication through shared per-sample image copies.

Limits and reported properties: `storageImageSampleCounts = pdev->info.supported_sample_counts & (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT)`.

Consumers: DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image.h](../src/kosmickrisp/vulkan/kk_image.h), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_image_view.h](../src/kosmickrisp/vulkan/kk_image_view.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_cmd_copy.c](../src/kosmickrisp/vulkan/kk_cmd_copy.c), [kk_cmd_meta.c](../src/kosmickrisp/vulkan/kk_cmd_meta.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk-writable-msaa.h](../src/kosmickrisp/vulkan/kk-writable-msaa.h), [kk-writable-msaa-layout.h](../src/kosmickrisp/vulkan/kk-writable-msaa-layout.h), [kk-writable-msaa-image.h](../src/kosmickrisp/vulkan/kk-writable-msaa-image.h), [kk-writable-msaa-view.h](../src/kosmickrisp/vulkan/kk-writable-msaa-view.h), [kk-writable-msaa-nir.h](../src/kosmickrisp/vulkan/kk-writable-msaa-nir.h), [kk-writable-msaa-meta.h](../src/kosmickrisp/vulkan/kk-writable-msaa-meta.h), [vk_render_pass.c](../src/vulkan/runtime/vk_render_pass.c), [vk_render_pass.h](../src/vulkan/runtime/vk_render_pass.h).

### Preserve multi-plane sampled descriptor slots

Vulkan contract: Preserve multi-plane sampled descriptor slots.



Scope: the Vulkan operation identified above. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan applications, DXVK, vkd3d-proton.

Implementation: [kk_maintenance11.h](../src/kosmickrisp/vulkan/kk_maintenance11.h), [kk_maintenance11_multiplane.h](../src/kosmickrisp/vulkan/kk_maintenance11_multiplane.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c).

### Runtime binding of embedded sampler metadata

Vulkan contract: Runtime binding of embedded sampler metadata.

Loads embedded sampler metadata addresses through stage bindings in the existing root instead of emitting device addresses into shader source.

Scope: descriptor heap shader compilation, root snapshots. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan descriptor heap pipelines.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_dgc.c](../src/kosmickrisp/vulkan/kk_cmd_dgc.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_descriptor_set.h](../src/kosmickrisp/vulkan/kk_descriptor_set.h), [kk_descriptor_heap_api.h](../src/kosmickrisp/vulkan/kk_descriptor_heap_api.h), [kk_descriptor_heap_nir.h](../src/kosmickrisp/vulkan/kk_descriptor_heap_nir.h), [kk_embedded_sampler_runtime.h](../src/kosmickrisp/vulkan/kk_embedded_sampler_runtime.h).

### Driver shader memory and disk caches

Vulkan contract: Driver shader memory and disk caches.

Live pipelines share compiled shader objects through Mesa's memory cache. Mesa's disk cache stores serialized shaders under the driver build identity.

Scope: pipeline compilation, shader serialization. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan pipelines.

Implementation: [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Portable embedded sampler shader cache records

Vulkan contract: Portable embedded sampler shader cache records.

Stores a versioned embedded binding schema and canonical sampler states with native shader code. Cache loads recreate device-local metadata tables before shader publication; runtime pointers are excluded. Compiler provenance rejects missing runtime reconstruction without changing live in-process shader reuse.

Scope: native shader serialization, native shader deserialization. Device support and operation restrictions are defined by the linked implementation.

Consumers: pipeline caches, shader binaries.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader_cache_bindings.h](../src/kosmickrisp/vulkan/kk_shader_cache_bindings.h).

### Layered block-texel view resource selection

Vulkan contract: Layered block-texel view resource selection.



Limits and reported properties: `VkPhysicalDeviceMaintenance6Properties.blockTexelViewCompatibleMultipleLayers = true`.

Consumers: Vulkan applications.

Implementation: [kk_layered_bc.c](../src/kosmickrisp/vulkan/kk_layered_bc.c), [kk_layered_bc.h](../src/kosmickrisp/vulkan/kk_layered_bc.h), [kk_layered_bc_nir.h](../src/kosmickrisp/vulkan/kk_layered_bc_nir.h), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_image_view.h](../src/kosmickrisp/vulkan/kk_image_view.h), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [meson.build](../src/kosmickrisp/vulkan/meson.build), [kk_layered_bc_render.h](../src/kosmickrisp/vulkan/kk_layered_bc_render.h), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_layered_bc_cube_nir.h](../src/kosmickrisp/vulkan/kk_layered_bc_cube_nir.h), [kk_mesh_command.h](../src/kosmickrisp/vulkan/kk_mesh_command.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Standard-shaped sparse 3D residency

Vulkan contract: Standard-shaped sparse 3D residency.

Standard 64 KiB 3D sparse cells and packed tails use shared canonical physical pages with logical residency and native backing overlap.

Limits and reported properties: `residencyStandard3DBlockShape = true`.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_standard_sparse_3d_layout.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_layout.h), [kk_standard_sparse_3d.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d.h), [kk_standard_sparse_3d_impl.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_impl.h), [kk_standard_sparse_3d_nir.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_nir.h), [kk_standard_sparse_3d_physical.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_physical.h), [kk_standard_sparse_3d_sample.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_sample.h), [kk_standard_sparse_3d_blit.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_blit.h), [kk_standard_sparse_3d_clear.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_clear.h), [kk_standard_sparse_3d_meta.h](../src/kosmickrisp/vulkan/kk_standard_sparse_3d_meta.h), [kk_sparse.h](../src/kosmickrisp/vulkan/kk_sparse.h), [kk_sparse.c](../src/kosmickrisp/vulkan/kk_sparse.c), [kk_sparse_pipeline.h](../src/kosmickrisp/vulkan/kk_sparse_pipeline.h), [kk_image.c](../src/kosmickrisp/vulkan/kk_image.c), [kk_image_view.c](../src/kosmickrisp/vulkan/kk_image_view.c), [kk_cmd_clear.c](../src/kosmickrisp/vulkan/kk_cmd_clear.c), [kk_device_memory.h](../src/kosmickrisp/vulkan/kk_device_memory.h), [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [kk_cmd_copy.c](../src/kosmickrisp/vulkan/kk_cmd_copy.c), [kk_cmd_meta.c](../src/kosmickrisp/vulkan/kk_cmd_meta.c), [kk_sampler_minmax.h](../src/kosmickrisp/vulkan/kk_sampler_minmax.h), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_nir_lower_sparse.c](../src/kosmickrisp/vulkan/kk_nir_lower_sparse.c), [vk_meta.h](../src/vulkan/runtime/vk_meta.h), [vk_meta_blit_resolve.c](../src/vulkan/runtime/vk_meta_blit_resolve.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Pixel-scope interlock transactions for multisampled fragments

Vulkan contract: `VK_EXT_fragment_shader_interlock`, `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderPixelInterlock`.

Capture completion schedules private continuation work without a host wait in queue submission. Private gates preserve later queue work and application synchronization until completion, and device loss releases native waits before callback drain.

Limits and reported properties: `VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT.fragmentShaderPixelInterlock = gpu_apple_family >= 7`.

Consumers: vkd3d-proton, ANGLE.

Implementation: [kk_interlock_submit.c](../src/kosmickrisp/vulkan/kk_interlock_submit.c), [kk_interlock_submit.h](../src/kosmickrisp/vulkan/kk_interlock_submit.h), [mtl_interlock_continuation.m](../src/kosmickrisp/bridge/mtl_interlock_continuation.m), [mtl_interlock_continuation.h](../src/kosmickrisp/bridge/mtl_interlock_continuation.h), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_sync.c](../src/kosmickrisp/vulkan/kk_sync.c), [kk_interlock_select.h](../src/kosmickrisp/vulkan/kk_interlock_select.h), [kk_interlock_quad_nir.h](../src/kosmickrisp/vulkan/kk_interlock_quad_nir.h), [kk_interlock_capture_nir.h](../src/kosmickrisp/vulkan/kk_interlock_capture_nir.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_queue.h](../src/kosmickrisp/vulkan/kk_queue.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_sync.h](../src/kosmickrisp/vulkan/kk_sync.h), [meson.build](../src/kosmickrisp/vulkan/meson.build), [meson.build](../src/kosmickrisp/bridge/meson.build), [mtl_interlock_continuation.c](../src/kosmickrisp/bridge/stubs/mtl_interlock_continuation.c), [vk_shader.h](../src/vulkan/runtime/vk_shader.h), [vk_pipeline.c](../src/vulkan/runtime/vk_pipeline.c).

### Logical sampler capacity and native canonical states

Vulkan contract: Logical sampler capacity and native canonical states.

Reserves canonical native tap states before acceleration consumes the application budget, validates native IDs against ordinary Metal argument encoding before publication, shares native materialization across logical devices, and retains original logical sampler parameters for shared sampling.

Scope: native sampler construction, sampler cache and table allocation. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, descriptor heaps.

Implementation: [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_sampler.h](../src/kosmickrisp/vulkan/kk_sampler.h), [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk-sampler-cap-keys.h](../src/kosmickrisp/vulkan/kk-sampler-cap-keys.h), [kk-sampler-cap-native.h](../src/kosmickrisp/vulkan/kk-sampler-cap-native.h), [kk-sampler-capacity.h](../src/kosmickrisp/vulkan/kk-sampler-capacity.h), [mtl_sampler.h](../src/kosmickrisp/bridge/mtl_sampler.h), [mtl_sampler.m](../src/kosmickrisp/bridge/mtl_sampler.m), [kk-sampler-id-check.h](../src/kosmickrisp/bridge/kk-sampler-id-check.h).

### Logical sampler parameter descriptor routes

Vulkan contract: Logical sampler parameter descriptor routes.

Sampler finalization receives explicit caller lock context. Virtual descriptor planes preserve their original filter metadata and share one immutable parameter address through sets, copied descriptors and heap metadata.

Scope: sampler finalization, common sampled and sampler descriptor preparation. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c).

### Shared virtual sampler parameters and footprints

Vulkan contract: Shared virtual sampler parameters and footprints.

Virtual sampler operations read immutable original parameters through one complete-metadata accessor. Sparse operations share the same contributing-tap footprint and retain actual color and residency together.

Scope: native sampler operations, shared reduction and border footprints. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan samplers, descriptor heaps.

Implementation: [kk-sampler-params-nir.h](../src/kosmickrisp/compiler/kk-sampler-params-nir.h), [msl_nir_lower_common.c](../src/kosmickrisp/compiler/msl_nir_lower_common.c), [msl_compute_derivatives.h](../src/kosmickrisp/compiler/msl_compute_derivatives.h), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_sparse_layout.h](../src/kosmickrisp/compiler/kk_sparse_layout.h), [kk-sampler-helper-flags.h](../src/kosmickrisp/compiler/kk-sampler-helper-flags.h), [kk-sampler-helpers-msl.h](../src/kosmickrisp/compiler/kk-sampler-helpers-msl.h), [kk-sampler-helpers-nir.h](../src/kosmickrisp/vulkan/kk-sampler-helpers-nir.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

### Preserve descriptor roots across unchanged binds

Vulkan contract: Preserve descriptor roots across unchanged binds.

Keep descriptor roots clean across identical set and dynamic-offset binds while preserving push descriptor invalidation and changed root addresses. Repeated single-set binds with no dynamic descriptors return before the general binding loop while preserving pending dirty state.

Scope: Command recording and submission. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan applications.

Implementation: [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

### Prepare generated graphics once for layered attachment readers

Vulkan contract: Prepare generated graphics once for layered attachment readers.

Expands ordered GPU-resident sequence records once and enters the shared producer/captured-raster boundary before binding logical layers. Keeps inactive records initialized without reading inactive token data and orders raster readers before scratch/heap reuse.

Scope: graphics generated commands with layered aliases. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton, Wine vkd3d.

Implementation: [kk_cmd_dgc.c](../src/kosmickrisp/vulkan/kk_cmd_dgc.c), [kk_dgc_poly.h](../src/kosmickrisp/vulkan/kk_dgc_poly.h), [kk_dgc_shader.h](../src/kosmickrisp/vulkan/kk_dgc_shader.h), [kk_dgc_layered.h](../src/kosmickrisp/vulkan/kk_dgc_layered.h).

### Avoid clearing unused fresh render color slots

Vulkan contract: Avoid clearing unused fresh render color slots.

Populate only active color attachments on a newly allocated pass descriptor. Preserve clearing of every old slot when a color mapping reuses a descriptor, along with all active attachment load/store/resolve setup. Layered physical-slice refreshes also retain clearing of previous slots.

Scope: Native render-pass descriptor setup, Dynamic color-attachment remapping. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan ordinary render passes.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_layered_bc_render.h](../src/kosmickrisp/vulkan/kk_layered_bc_render.h).

## Shader compilation and arithmetic

### FP32 denorm preservation

Vulkan contract: `VK_KHR_shader_float_controls`.

Preserves explicit FP32 subnormal arithmetic through bit-normalized operands, TwoProduct/fma residuals and a single integer RTE pack; normal-range operations use native arithmetic. FP16 and shaders without DenormPreserve 32 retain their existing paths.

Limits and reported properties: `VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat32 = true`.

Consumers: vkd3d-proton, Vulkan shaders requesting DenormPreserve 32.

Implementation: [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_fp32_preserve.h](../src/kosmickrisp/compiler/kk_fp32_preserve.h), [kk_fp32_preserve_msl.h](../src/kosmickrisp/compiler/kk_fp32_preserve_msl.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Explicit zero-initialize allocation flag

Vulkan contract: Explicit zero-initialize allocation flag.

The zero-initialize allocation flag clears the entire shared BO on the CPU or fills the private BO with a Metal blit and waits for completion before returning memory. Unflagged allocations and imports receive no clear or queue work. ZERO_INITIALIZED image storage remains unoptimized so cleared bits represent zero texels.

Scope: flagged non-imported device-memory allocation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [mtl_buffer.h](../src/kosmickrisp/bridge/mtl_buffer.h), [mtl_buffer.m](../src/kosmickrisp/bridge/mtl_buffer.m).

### Fragment shader quad control

Vulkan contract: `VK_KHR_shader_quad_control`, `VkPhysicalDeviceShaderQuadControlFeaturesKHR.shaderQuadControl`.

Lowers fragment quad votes to active-lane ballots in the shared subgroup pass and retains full-quad requirements through later info gathering. This gives divergent votes consistent participation on all OS versions.

Scope: extension and feature reporting, subgroup NIR lowering. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton shader model 6.7, Fragment shaders using full quads or quad-uniform derivative control flow.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [msl_nir_lower_subgroups.c](../src/kosmickrisp/compiler/msl_nir_lower_subgroups.c), [kk_quad_control.h](../src/kosmickrisp/compiler/kk_quad_control.h).

### Shared shader compilation options

Vulkan contract: Shared shader compilation options.

Carries the existing transport map and native compilation context through one options structure. Null selects zero defaults; shader lowering and source emission retain their existing behavior.

Scope: shader compilation, per-vertex transport, MSL translation. Device support and operation restrictions are defined by the linked implementation.

Consumers: KosmicKrisp shader compiler, Shared per-vertex transport, Native stage and fragment-output compilation.

Implementation: [kk_compile_options.h](../src/kosmickrisp/vulkan/kk_compile_options.h), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [nir_to_msl.h](../src/kosmickrisp/compiler/nir_to_msl.h), [kk_argument_table.h](../src/kosmickrisp/libkk/kk_argument_table.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Shared generated graphics wrapper and interlock bindings

Vulkan contract: `VK_EXT_device_generated_commands`.

Captures selected linked-stage wrapper addresses after the indirect-mode graphics flush and binds them per ICB command with the separate interlock guard through the shared slot and capacity declaration.

Scope: generated graphics, indirect-count graphics. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_dgc.h](../src/kosmickrisp/vulkan/kk_dgc.h), [kk_cmd_dgc.c](../src/kosmickrisp/vulkan/kk_cmd_dgc.c), [kk_dgc.m](../src/kosmickrisp/vulkan/kk_dgc.m), [kk_dgc_shader.h](../src/kosmickrisp/vulkan/kk_dgc_shader.h).

### Native FP16 and FP32 subgroup matrix fragments

Vulkan contract: Native FP16 and FP32 subgroup matrix fragments.

Lowers 8x8 and 16x16 subgroup matrices through shared native Metal 8x8 fragments, preserving opaque per-lane element distribution.

Scope: compute shaders, NIR to MSL. Device support and operation restrictions are defined by the linked implementation.

Consumers: vkd3d-proton cooperative-matrix shaders.

Implementation: [nir_intrinsics.py](../src/compiler/nir/nir_intrinsics.py), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [msl_type_inference.c](../src/kosmickrisp/compiler/msl_type_inference.c), [kk_matrix_tile.h](../src/kosmickrisp/compiler/kk_matrix_tile.h).

### Explicit fixed-width compute subgroups

Vulkan contract: Explicit fixed-width compute subgroups.

Reports compute as accepting explicit subgroup size32, using the existing Mesa pipeline selection and native Metal SIMD width. Default pipelines and full-subgroup behavior use the same path.

Limits and reported properties: `requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT`.

Consumers: vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Scalar lowering for long shader vectors

Vulkan contract: `VK_EXT_shader_long_vector`, `VkPhysicalDeviceShaderLongVectorFeaturesEXT.longVector`.

Represents long shader vectors as scalar arrays while sharing ordinary arithmetic and memory lowering.

Limits and reported properties: `maxVectorComponents = 1024`.

Consumers: vkd3d-proton.

Implementation: [spirv_to_nir.c](../src/compiler/spirv/spirv_to_nir.c), [vtn_alu.c](../src/compiler/spirv/vtn_alu.c), [vtn_glsl450.c](../src/compiler/spirv/vtn_glsl450.c), [vtn_subgroup.c](../src/compiler/spirv/vtn_subgroup.c), [vtn_constants.c](../src/compiler/spirv/vtn_constants.c), [vtn_private.h](../src/compiler/spirv/vtn_private.h), [vtn_long_vector.h](../src/compiler/spirv/vtn_long_vector.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Software binary64 shader arithmetic

Vulkan contract: `VkPhysicalDeviceFeatures.shaderFloat64`, `VkPhysicalDeviceShaderFmaFeaturesKHR.shaderFmaFloat64`.

Inlines the pinned integer software library into shaders with binary64 arithmetic. Exact division and fused operations retain their remainder before one round-to-nearest-even pack. Conversion and frexp helpers preserve denormals independently of FP16 and FP32 modes. Shaders without binary64 skip the library.

Limits and reported properties: `VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat64 = true`; `VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat64 = true`; `VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat64 = true`.

Consumers: vkd3d-proton double-precision DXIL shaders, DXVK double-precision DXBC shaders, Vulkan binary64 shaders.

Implementation: [float64.glsl](../src/compiler/glsl/float64.glsl), [nir_softfloat.h](../src/compiler/nir/nir_softfloat.h), [kk_soft_fp64.h](../src/kosmickrisp/compiler/kk_soft_fp64.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [nir_lower_subgroups.c](../src/compiler/nir/nir_lower_subgroups.c), [kk_soft_fp64_subgroups.h](../src/compiler/nir/kk_soft_fp64_subgroups.h), [meson.build](../src/kosmickrisp/compiler/meson.build).

### Subgroup cooperative matrices and FP8 conversions

Vulkan contract: `VK_KHR_cooperative_matrix`, `VK_EXT_shader_float8`, `cooperativeMatrix`, `shaderFloat8`, `shaderFloat8CooperativeMatrix`.

Composes native 8x8 fragments into 16x16 subgroup matrices, addresses matrix memory using original pointee strides, and performs FP8 conversion on the GPU.

Limits and reported properties: `cooperativeMatrixSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT`.

Consumers: vkd3d-proton AGS WMMA_FP8.

Implementation: [kk_float8.h](../src/kosmickrisp/compiler/kk_float8.h), [kk_float8_msl.h](../src/kosmickrisp/compiler/kk_float8_msl.h), [kk_matrix_tile.h](../src/kosmickrisp/compiler/kk_matrix_tile.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c), [kk_cooperative_matrix.h](../src/kosmickrisp/vulkan/kk_cooperative_matrix.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c).

### Integer arithmetic wraparound in Metal shaders

Vulkan contract: Integer arithmetic wraparound in Metal shaders.

Emit modular integer addition, subtraction, multiplication, negation and three-operand arithmetic through unsigned types at the original width, preserving the destination bit pattern. Signed comparison and division retain their original semantics.

Scope: MSL integer arithmetic emission. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan integer shaders, Software tessellation and geometry indexing.

Implementation: [kk_integer_wrap.h](../src/kosmickrisp/compiler/kk_integer_wrap.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Select Metal library language from shader operations

Vulkan contract: Select Metal library language from shader operations.

Compiles libraries at MSL 4.0 unless lowered NIR contains threadgroup floating-point atomic addition. Retains the OS-derived supported language and feature advertisement, and preserves the requirement through shader copies and serialization.

Scope: shader compilation, shader serialization, precompiled driver programs. Device support and operation restrictions are defined by the linked implementation.

Consumers: KosmicKrisp shader compiler.

Implementation: [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_mesh_pipeline.h](../src/kosmickrisp/vulkan/kk_mesh_pipeline.h), [kk_device_lib.c](../src/kosmickrisp/vulkan/kk_device_lib.c).

### Copy constant matrix coordinate defaults

Vulkan contract: Copy constant matrix coordinate defaults.

Copy the exact binary16 coordinate ramp0..63 into the existing command-owned root instead of converting the same64 constants on every recording. Storage, layout and initialization order remain unchanged.

Scope: Command-buffer descriptor-root initialization. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan command recording.

Implementation: [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

## Presentation and platform integration

### Direct Wine entry points and promoted command lookup

Vulkan contract: Direct Wine entry points and promoted command lookup.

Adds Apple flat exports and a generated-table promoted-core fallback only at the direct GIPA boundary. Normal ICD dispatch remains upstream.

Scope: direct library lookup. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_instance.c](../src/kosmickrisp/vulkan/kk_instance.c), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c).

### Metal presentation scaling and gravity

Vulkan contract: `VK_EXT_surface_maintenance1`, `VK_EXT_swapchain_maintenance1`, `VK_KHR_surface_maintenance1`, `VK_KHR_swapchain_maintenance1`.

Uses the existing GPU present blit for pixel-preserving, aspect-fit and independent-axis scaling with gravity, clipping, borders and live surface resizing. Deliberately scaled extents do not produce extent-only SUBOPTIMAL.

Limits and reported properties: `VkSurfacePresentScalingCapabilitiesKHR.supportedPresentScaling = ["ONE_TO_ONE", "ASPECT_RATIO_STRETCH", "STRETCH"]`; `VkSurfacePresentScalingCapabilitiesKHR.supportedPresentGravityX = ["MIN", "CENTERED", "MAX"]`; `VkSurfacePresentScalingCapabilitiesKHR.supportedPresentGravityY = ["MIN", "CENTERED", "MAX"]`.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [wsi_common_metal.c](../src/vulkan/wsi/wsi_common_metal.c), [wsi_common_metal_layer.h](../src/vulkan/wsi/wsi_common_metal_layer.h), [wsi_common_metal_layer.m](../src/vulkan/wsi/wsi_common_metal_layer.m), [wsi_metal_scaling.h](../src/vulkan/wsi/wsi_metal_scaling.h), [kk_wsi.c](../src/kosmickrisp/vulkan/kk_wsi.c).

### Separate drawable waits from application submissions

Vulkan contract: Separate drawable waits from application submissions.

Lazily allocates a private WSI queue with a command pool, retains semaphore dependencies and drains it before shared meta teardown. Allocation failure retains the application queue.

Scope: Metal surface and swapchain presentation. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_wsi.c](../src/kosmickrisp/vulkan/kk_wsi.c).

### Native headless swapchains on macOS

Vulkan contract: `VK_EXT_headless_surface`.

Enables the existing headless surface API and uses optimal device images on macOS without DRM, a layer, drawables or a CPU image copy.

Scope: Metal surface and swapchain presentation. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_instance.c](../src/kosmickrisp/vulkan/kk_instance.c), [wsi_common_private.h](../src/vulkan/wsi/wsi_common_private.h), [wsi_common.c](../src/vulkan/wsi/wsi_common.c), [wsi_common_headless.c](../src/vulkan/wsi/wsi_common_headless.c).

### Incremental presentation region hints

Vulkan contract: `VK_KHR_incremental_present`.

Exposes damage-region hints through existing Mesa parsing and correct complete-image Metal presentation.

Scope: Metal surface and swapchain presentation. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Depth-bias representations after polygon clipping

Vulkan contract: `VK_EXT_depth_bias_control`, `VkPhysicalDeviceDepthBiasControlFeaturesEXT.depthBiasControl`, `VkPhysicalDeviceDepthBiasControlFeaturesEXT.leastRepresentableValueForceUnormRepresentation`, `VkPhysicalDeviceDepthBiasControlFeaturesEXT.floatRepresentation`, `VkPhysicalDeviceDepthBiasControlFeaturesEXT.depthBiasExact`.

FLOAT, FORCE_UNORM and exact format bias share the Poly clipping path. The constant and slope terms are clamped together after clipping. FILL emits a triangle fan, LINE emits boundary segments and POINT retains square expansion; ordinary consumers retain native bias.

Scope: Poly capture and post-clip polygon output, shader compilation and pipeline cache keys, native depth-bias suppression and viewport depth clamp. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK D3D9, DXVK D3D11, vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [geometry_capture.h](../src/poly/geometry_capture.h), [kk_polygon_nir.h](../src/kosmickrisp/vulkan/kk_polygon_nir.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl).

### Metal drawable presentation timing

Vulkan contract: `VK_GOOGLE_display_timing`.

Schedules requested host times through presentAtTime and reports only nonzero drawable presentedTime through the common timing history. CoreVideo reports the attached display nominal refresh duration; callbacks are excluded from swapchain destruction.

Scope: untimed drawable presentation, present-wait completion. Device support and operation restrictions are defined by the linked implementation.

Consumers: native Vulkan presentation clients.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [meson.build](../src/vulkan/wsi/meson.build), [wsi_common_metal.c](../src/vulkan/wsi/wsi_common_metal.c), [wsi_common_metal_layer.h](../src/vulkan/wsi/wsi_common_metal_layer.h), [wsi_common_metal_layer.m](../src/vulkan/wsi/wsi_common_metal_layer.m).

## Command submission and pipeline state

### Enable the existing custom border color implementation

Vulkan contract: `VK_EXT_custom_border_color`, `VK_EXT_border_color_swizzle`, `VkPhysicalDeviceBorderColorSwizzleFeaturesEXT.borderColorSwizzle`, `VkPhysicalDeviceBorderColorSwizzleFeaturesEXT.borderColorSwizzleFromImage`, `VkPhysicalDeviceCustomBorderColorFeaturesEXT.customBorderColors`, `VkPhysicalDeviceCustomBorderColorFeaturesEXT.customBorderColorWithoutFormat`.

Sets KK_EXPERIMENTAL_CUSTOM_BORDER by default and uses upstream existing sampler and shader implementation; no new border-color algorithm.

Limits and reported properties: `VkPhysicalDeviceBorderColorSwizzleFeaturesEXT.borderColorSwizzleFromImage = false`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_debug.c](../src/kosmickrisp/vulkan/kk_debug.c).

### Preserve opaque-black depth comparison borders

Vulkan contract: Preserve opaque-black depth comparison borders.

Keeps the native zero depth border for predefined opaque-black comparison while retaining the RGBA4 colour workaround.

Scope: standard opaque-black comparison sampler creation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton.

Implementation: [kk_sampler.c](../src/kosmickrisp/vulkan/kk_sampler.c).

### Single-texel logical buffer views over aligned Metal textures

Vulkan contract: `VK_EXT_texel_buffer_alignment`.

Expands storage-image descriptors from 8 to 16 bytes, aligns native offset down, carries prefix/count and preserves logical bounds and query size. Buffer capacity reserves prefix space.

Limits and reported properties: `VkPhysicalDeviceTexelBufferAlignmentProperties.storageTexelBufferOffsetSingleTexelAlignment = true`; `VkPhysicalDeviceTexelBufferAlignmentProperties.uniformTexelBufferOffsetSingleTexelAlignment = true`; `VkPhysicalDeviceLimits.maxTexelBufferElements = 16384 * 16384 - KK_MIN_TEXEL_BUFFER_ALIGNMENT`.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_descriptor_types.h](../src/kosmickrisp/vulkan/kk_descriptor_types.h), [kk_buffer_view.h](../src/kosmickrisp/vulkan/kk_buffer_view.h), [kk_buffer_view.c](../src/kosmickrisp/vulkan/kk_buffer_view.c), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Run compiler workaround and optimization passes on each variant

Vulkan contract: Run compiler workaround and optimization passes on each variant.

After cloning the variant s, run msl_preprocess_nir_workarounds and msl_optimize_nir on s rather than the shared nir.

Scope: every offline library-kernel variant. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_clc.c](../src/kosmickrisp/clc/kk_clc.c).

### Use thread dimensions for precompiled indirect-local grids

Vulkan contract: Use thread dimensions for precompiled indirect-local grids.

Uses mtl_dispatch_threads_with_indirect_buffer for indirect-local mode while preserving direct and indirect-group paths.

Scope: all precompiled helper dispatches using KK_GRID_INDIRECT_LOCAL. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

### Select passthrough adjacency vertices from actual draw topology

Vulkan contract: Select passthrough adjacency vertices from actual draw topology.

Uses reduced primitive class and runtime topology to select adjacency vertices rather than initial dynamic pipeline topology; avoids another raster pipeline variant.

Scope: passthrough GS output class, shared Poly line/triangle adjacency vertex selection. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [poly_nir_lower_gs.c](../src/poly/nir/poly_nir_lower_gs.c).

### Empty whole-size texel views without native textures

Vulkan contract: Empty whole-size texel views without native textures.

Valid views with fewer than one remaining texel use a zero descriptor and allocate no Metal texture.

Scope: empty texel-buffer views. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, Vulkan texel-buffer consumers.

Implementation: [kk_buffer_view.c](../src/kosmickrisp/vulkan/kk_buffer_view.c), [kk_descriptor_set.c](../src/kosmickrisp/vulkan/kk_descriptor_set.c).

### Double or triple buffered Metal swapchains

Vulkan contract: Double or triple buffered Metal swapchains.

Allows two or three swapchain images using CAMetalLayer maximumDrawableCount, retaining FIFO and IMMEDIATE semantics.

Limits and reported properties: `VkSurfaceCapabilitiesKHR.minImageCount = 2`; `VkSurfaceCapabilitiesKHR.maxImageCount = 3`.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, winevulkan.

Implementation: [wsi_common_metal.c](../src/vulkan/wsi/wsi_common_metal.c).

### RGB32 uniform texel buffer views

Vulkan contract: RGB32 uniform texel buffer views.

RGB32 float, unsigned and signed buffer SRVs fetch three native R32 components while retaining logical offsets, lengths and empty views. Storage RGB32 access is not advertised.

Scope: format properties, shared descriptor and Metal representation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_buffer_view.c](../src/kosmickrisp/vulkan/kk_buffer_view.c), [kk_nir_lower_descriptors.c](../src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c).

### Independent compute and transfer queue families

Vulkan contract: Independent compute and transfer queue families.

Adds compute/transfer and transfer families with one queue each, and a second queue in graphics family 0. Each requested queue owns an independent Metal 4 command queue. Existing submission, semaphore, resource-barrier and private presentation paths are shared.

Scope: graphics family 0, queue submission, shared-event synchronization, private present queue. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, Wine vkd3d, Vulkan.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_wsi.c](../src/kosmickrisp/vulkan/kk_wsi.c), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c).

### ALU temporary declarations without redundant initialization

Vulkan contract: ALU temporary declarations without redundant initialization.

ALU SSA temporaries omit an initial zero assignment because the defining operation assigns the whole value. Guarded intrinsic and texture results keep zero initialization.

Scope: MSL source generation. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Device address transfer and binding commands

Vulkan contract: `VK_KHR_device_address_commands`, `deviceAddressCommands`.

Shares the Vulkan meta address-copy backend across handle and address commands, stages image-copy rows through the existing format paths, and includes address-range barriers and transform feedback address bindings.

Scope: buffer copy, buffer fill and update, buffer and image copies, transform feedback. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan address-based command recording.

Implementation: [kk_cmd_copy.c](../src/kosmickrisp/vulkan/kk_cmd_copy.c), [kk_cmd_meta.c](../src/kosmickrisp/vulkan/kk_cmd_meta.c), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_address_copy.h](../src/kosmickrisp/vulkan/kk_address_copy.h).

### GPU preprocessing and generated graphics and compute commands

Vulkan contract: `VK_EXT_device_generated_commands`, `deviceGeneratedCommands`, `dynamicGeneratedPipelineLayout`.

Expands token streams into GPU root and per-draw records, then encodes Metal indirect command buffers with sequence-specific push data and buffer bindings.

Limits and reported properties: `maxDrawIndirectCount = 1u << 30`; `maxIndirectSequenceCount = 1u << 20`; `supportedIndirectCommandsShaderStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT`.

Consumers: vkd3d-proton.

Implementation: [kk_cmd_dgc.c](../src/kosmickrisp/vulkan/kk_cmd_dgc.c), [kk_dgc.h](../src/kosmickrisp/vulkan/kk_dgc.h), [kk_dgc.m](../src/kosmickrisp/vulkan/kk_dgc.m), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [mtl_compiler.m](../src/kosmickrisp/bridge/mtl_compiler.m), [meson.build](../src/kosmickrisp/vulkan/meson.build), [kk_dgc_shader.h](../src/kosmickrisp/vulkan/kk_dgc_shader.h), [kk_dgc_indirect.h](../src/kosmickrisp/vulkan/kk_dgc_indirect.h), [kk_dgc.cl](../src/kosmickrisp/libkk/kk_dgc.cl), [meson.build](../src/kosmickrisp/libkk/meson.build), [kk_dgc_poly.h](../src/kosmickrisp/vulkan/kk_dgc_poly.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [kk_vertex_input.h](../src/kosmickrisp/vulkan/kk_vertex_input.h), [mtl_compiler.h](../src/kosmickrisp/bridge/mtl_compiler.h), [mtl_compiler.c](../src/kosmickrisp/bridge/stubs/mtl_compiler.c).

### Additional independent graphics queue family

Vulkan contract: Additional independent graphics queue family.

Appends a second graphics/compute/transfer family with one independently initialized native queue while retaining the existing family indices and queue counts. Shared buffer/image storage and synchronization support exclusive transfers and concurrent resources across graphics families.

Limits and reported properties: `queueFamilies = [{"index": 0, "operations": ["graphics", "compute", "transfer"], "queueCount": 2}, {"index": 1, "operations": ["compute", "transfer"], "queueCount": 1}, {"index": 2, "operations": ["transfer"], "queueCount": 1}, {"index": 3, "operations": ["graphics", "compute", "transfer"], "queueCount": 1}]`.

Consumers: Vulkan applications using graphics ownership transfers between queue families, DXVK, vkd3d-proton.

Implementation: [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c), [kk_physical_device.h](../src/kosmickrisp/vulkan/kk_physical_device.h), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h).

### Four-byte storage-buffer offsets

Vulkan contract: Four-byte storage-buffer offsets.

Exact four-byte byte offsets and descriptor ranges use inferred scalar alignment for global memory accesses.

Limits and reported properties: `VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment = KK_MIN_SSBO_ALIGNMENT`.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_private.h](../src/kosmickrisp/vulkan/kk_private.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Specialize inactive framebuffer-read inputs

Vulkan contract: Specialize inactive framebuffer-read inputs.

Function constants remove absent framebuffer-read fields from the Metal interface. Native pipelines specialize reads to actual attachment activity while retaining the unspecialized descriptor and sharing shader code, pre-raster programs and existing format/sample/indirect cache keys.

Scope: framebuffer-read input emission, cached attachment pipeline variants. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton.

Implementation: [msl_iomap.c](../src/kosmickrisp/compiler/msl_iomap.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_unused_attachments.h](../src/kosmickrisp/vulkan/kk_unused_attachments.h), [mtl_compiler.h](../src/kosmickrisp/bridge/mtl_compiler.h), [mtl_compiler.m](../src/kosmickrisp/bridge/mtl_compiler.m).

### Shared native allocation residency lifetime

Vulkan contract: Shared native allocation residency lifetime.

BO aliases share one native residency registration until the final alias is destroyed. Unique allocations remain constant-time and allocate no extra record. Sparse virtual resource registrations are unchanged.

Scope: ordinary and imported BO registration, native heap lifetime. Device support and operation restrictions are defined by the linked implementation.

Consumers: DXVK, vkd3d-proton, native Metal interop.

Implementation: [kk_bo.h](../src/kosmickrisp/vulkan/kk_bo.h), [kk_bo.c](../src/kosmickrisp/vulkan/kk_bo.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_device_memory.c](../src/kosmickrisp/vulkan/kk_device_memory.c), [mtl_residency_set.h](../src/kosmickrisp/bridge/mtl_residency_set.h), [mtl_residency_set.m](../src/kosmickrisp/bridge/mtl_residency_set.m).

### Post-grid degenerate conservative triangles

Vulkan contract: Post-grid degenerate conservative triangles.

Preserve post-grid conservative degenerates with backface culling and original provoking attributes and depth.

Limits and reported properties: `degenerateTrianglesRasterized = true`.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_device_lib.c](../src/kosmickrisp/vulkan/kk_device_lib.c), [kk_conservative_canonical.h](../src/kosmickrisp/libkk/kk_conservative_canonical.h), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_conservative_fs_link.h](../src/kosmickrisp/vulkan/kk_conservative_fs_link.h), [kk_polygon.cl](../src/kosmickrisp/libkk/kk_polygon.cl), [kk_conservative.cl](../src/kosmickrisp/libkk/kk_conservative.cl), [kk_conservative.h](../src/kosmickrisp/libkk/kk_conservative.h), [kk_conservative_nir.h](../src/kosmickrisp/vulkan/kk_conservative_nir.h), [kk_conservative_fs_prolog.c](../src/kosmickrisp/vulkan/kk_conservative_fs_prolog.c), [kk_polygon_command.h](../src/kosmickrisp/vulkan/kk_polygon_command.h), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Conservative full-fragment inner coverage

Vulkan contract: Conservative full-fragment inner coverage.

Evaluate whole logical-fragment inner coverage from the original clipped polygon, independent of masks and depth.

Limits and reported properties: `fullyCoveredFragmentShaderInputVariable = true`.

Consumers: DXVK, vkd3d-proton.

Implementation: [kk_conservative.h](../src/kosmickrisp/libkk/kk_conservative.h), [kk_conservative.cl](../src/kosmickrisp/libkk/kk_conservative.cl), [kk_conservative_inner.cl](../src/kosmickrisp/libkk/kk_conservative_inner.cl), [kk_conservative_inner_nir.h](../src/kosmickrisp/vulkan/kk_conservative_inner_nir.h), [kk_conservative_inner_msl.h](../src/kosmickrisp/vulkan/kk_conservative_inner_msl.h), [kk_conservative_nir.h](../src/kosmickrisp/vulkan/kk_conservative_nir.h), [kk_conservative_fs_link.h](../src/kosmickrisp/vulkan/kk_conservative_fs_link.h), [kk_conservative_fs_prolog.c](../src/kosmickrisp/vulkan/kk_conservative_fs_prolog.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_physical_device.c](../src/kosmickrisp/vulkan/kk_physical_device.c).

### Fragment offset interpolation through screen-space planes

Vulkan contract: Fragment offset interpolation through screen-space planes.

Evaluates fixed-location float32 offset inputs in primitive-corner and canonical interfaces from entry-hoisted affine numerator and reciprocal-W planes. Reciprocal-W pole quads retain native offset interpolation. Ordinary native interfaces, centroid/sample and dynamically selected input paths retain their existing interpolation.

Scope: fragment offset interpolation, primitive-corner and canonical fragment inputs. Device support and operation restrictions are defined by the linked implementation.

Consumers: ANGLE GLES 3.2, DXVK, vkd3d-proton, Wine vkd3d.

Implementation: [kk_fragment_offset_planes.h](../src/kosmickrisp/vulkan/kk_fragment_offset_planes.h), [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c).

### Native block alias texture construction

Vulkan contract: Native block alias texture construction.

Retains a canonical native texture or creates a typed alias over an existing image plane, then creates the requested sampled and storage views. Callers retain the backing image and memory through native view retirement.

Scope: native image creation. Device support and operation restrictions are defined by the linked implementation.

Consumers: layered block views, sparse three-dimensional images.

Implementation: [kk_native_block_alias.h](../src/kosmickrisp/vulkan/kk_native_block_alias.h), [kk_native_block_alias.c](../src/kosmickrisp/vulkan/kk_native_block_alias.c), [meson.build](../src/kosmickrisp/vulkan/meson.build).

### Cached Poly Layer replay

Vulkan contract: Cached Poly Layer replay.

Rasterizes cached pre-raster outputs into selected logical attachment layers with original producer execution and a nullable selector in a dedicated argument-table entry. Ordinary per-draw uploads retain their original extent.

Scope: Poly capture and generated raster readers. Device support and operation restrictions are defined by the linked implementation.

Consumers: Layered attachment aliases.

Implementation: [kk_poly_layer_replay_types.h](../src/kosmickrisp/vulkan/kk_poly_layer_replay_types.h), [kk_poly_layer_replay_nir.h](../src/kosmickrisp/vulkan/kk_poly_layer_replay_nir.h), [kk_poly_layer_replay_command.h](../src/kosmickrisp/vulkan/kk_poly_layer_replay_command.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_compile_options.h](../src/kosmickrisp/vulkan/kk_compile_options.h), [kk_argument_table.h](../src/kosmickrisp/libkk/kk_argument_table.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_dgc.m](../src/kosmickrisp/vulkan/kk_dgc.m), [kk_dgc_shader.h](../src/kosmickrisp/vulkan/kk_dgc_shader.h), [nir_to_msl.c](../src/kosmickrisp/compiler/nir_to_msl.c).

### Host-deferred continuation submission

Vulkan contract: Host-deferred continuation submission.

Retain a Mesa queue submission behind a fresh private continuation point, with on-demand host submission and committed semaphore values.

Scope: the Vulkan operation identified above. Device support and operation restrictions are defined by the linked implementation.

Consumers: fragment interlock continuation, deferred task and mesh dispatch.

Implementation: [vk_queue.c](../src/vulkan/runtime/vk_queue.c), [vk_queue.h](../src/vulkan/runtime/vk_queue.h), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c), [kk_queue.h](../src/kosmickrisp/vulkan/kk_queue.h), [kk_queue_deferred.h](../src/kosmickrisp/vulkan/kk_queue_deferred.h), [kk_sync.c](../src/kosmickrisp/vulkan/kk_sync.c), [kk_sync.h](../src/kosmickrisp/vulkan/kk_sync.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c).

### Retire native command allocator storage with its command buffer

Vulkan contract: Retire native command allocator storage with its command buffer.

Keep native command allocators exclusive to executable Vulkan command buffers, including split mesh segments, and reset and return their storage only when the recording is legally reset or destroyed. Native command buffers retain their backing allocator across pool reset and return prior storage before replacing that reference. Pool cleanup releases cached command buffers before allocators; trim shares the cleanup path. Recorded native buffers relinquish their storage before allocator reset; implicit command begin resets first, then acquires the next native buffer. Allocator pooling remains available.

Scope: Command recording and submission. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan applications.

Implementation: [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_pool.c](../src/kosmickrisp/vulkan/kk_cmd_pool.c), [mtl_command_buffer.m](../src/kosmickrisp/bridge/mtl_command_buffer.m).

### Coarse fragment execution

Vulkan contract: Coarse fragment execution.

Retains linked fragment source and its actual interface through the common shader lifetime without compiling eager coarse Metal functions. Native coverage replay and the shared lazy artifact cache preserve ordinary argument-table interfaces. Vulkan fragment-shading-rate features remain unavailable.

Scope: the Vulkan operation identified above. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan fragment execution.

Implementation: [kk_fragment_shading_rate.c](../src/kosmickrisp/compiler/kk_fragment_shading_rate.c), [kk_fragment_shading_rate.h](../src/kosmickrisp/compiler/kk_fragment_shading_rate.h), [meson.build](../src/kosmickrisp/compiler/meson.build), [mtl_fragment_shading_rate.m](../src/kosmickrisp/bridge/mtl_fragment_shading_rate.m), [mtl_fragment_shading_rate.h](../src/kosmickrisp/bridge/mtl_fragment_shading_rate.h), [meson.build](../src/kosmickrisp/bridge/meson.build), [kk_fragment_shading_rate_types.h](../src/kosmickrisp/libkk/kk_fragment_shading_rate_types.h), [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c), [kk_fragment_shading_rate_pipeline.h](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_pipeline.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_shader.h](../src/kosmickrisp/vulkan/kk_shader.h), [kk_fragment_shading_rate_cache.h](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_cache.h), [kk_fragment_shading_rate_cache_impl.h](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_cache_impl.h), [kk_compile_options.h](../src/kosmickrisp/vulkan/kk_compile_options.h), [kk_nir_lower_per_vertex.c](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c), [kk_nir_lower_per_vertex.h](../src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.h), [kk_fragment_shading_rate_command.h](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_command.h), [kk_fragment_shading_rate_command.c](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_command.c), [meson.build](../src/kosmickrisp/vulkan/meson.build), [kk_fragment_shading_rate_resources.h](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_resources.h), [kk_fragment_shading_rate_resources.c](../src/kosmickrisp/vulkan/kk_fragment_shading_rate_resources.c), [mtl_vrs_mesh_types.h](../src/kosmickrisp/bridge/mtl_vrs_mesh_types.h), [mtl_vrs_mesh_prepare.h](../src/kosmickrisp/bridge/mtl_vrs_mesh_prepare.h).

### Finish native fragment alternatives through shared lowering

Vulkan contract: Finish native fragment alternatives through shared lowering.

Run the shared fragment finishing path exactly once for native fragment alternatives using the original robustness and descriptor layouts. Main fragment programs retain their existing source lifetime.

Scope: Fragment epilogue and descriptor lowering, Native raster pipeline compilation. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan graphics pipelines with native raster alternatives.

Implementation: [kk_shader.c](../src/kosmickrisp/vulkan/kk_shader.c).

### Commit residency membership only after mutation

Vulkan contract: Commit residency membership only after mutation.

Track execution residency membership changes under the existing mutex and omit unchanged Metal residency-set commits. Residency requests, queue attachment and priority residency retain their existing behavior. Nonpageable devices request persistent residency once; subsequent dirty commits update the resident set. Pageable and high-priority residency paths are preserved.

Scope: Command recording and submission. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan applications.

Implementation: [kk_device.h](../src/kosmickrisp/vulkan/kk_device.h), [kk_device.c](../src/kosmickrisp/vulkan/kk_device.c), [kk_sparse.c](../src/kosmickrisp/vulkan/kk_sparse.c).

### Omit unchanged root argument-table address writes

Vulkan contract: Omit unchanged root argument-table address writes.

Omit unchanged root-address writes to the same native argument table. Command reset and a replacement argument table force an initial bind; internal compute root restoration retains its existing behavior.

Scope: Draw and compute argument table binding. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan applications.

Implementation: [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

### Skip repeated reset of initial command buffers

Vulkan contract: Skip repeated reset of initial command buffers.

An initial command buffer with no reset flags already has no recording to discard. Preserve full cleanup for recording, executable and invalid buffers and all resource-release resets.

Scope: Command-buffer reset, Submission retirement. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan command-buffer users.

Implementation: [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c).

### Selected command depth and stencil state

Vulkan contract: Selected command depth and stencil state.

Shares actual dynamic or attachment-keyed native state selection and exposes an immutable command-owned snapshot. Optional bypass and read-only states use the existing native state compiler; snapshots retain their handles through command completion without changing descriptor or shader ABIs.

Scope: ordinary depth/stencil selection, CPU attachment layout metadata, command reset and destroy. Device support and operation restrictions are defined by the linked implementation.

Consumers: Internal coverage and replay encoders.

Implementation: [kk_depth_stencil_state.c](../src/kosmickrisp/vulkan/kk_depth_stencil_state.c), [kk_depth_stencil_state.h](../src/kosmickrisp/vulkan/kk_depth_stencil_state.h), [kk_cmd_buffer.h](../src/kosmickrisp/vulkan/kk_cmd_buffer.h), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c), [meson.build](../src/kosmickrisp/vulkan/meson.build).

### Maintain command upload pool cache count

Vulkan contract: Maintain command upload pool cache count.

Initialize the free upload BO count with the empty list and clear it when cached BOs are destroyed, so reuse and cache-capacity decisions follow the actual list.

Scope: Command upload BO reuse, Command pool creation and trim. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan command-buffer upload storage.

Implementation: [kk_cmd_pool.c](../src/kosmickrisp/vulkan/kk_cmd_pool.c).

### Encode direct compute dispatches as threadgroups

Vulkan contract: Encode direct compute dispatches as threadgroups.

Pass Vulkan workgroup counts directly to Metal dispatchThreadgroups. Preserve local size, base-group root data, statistics, predication and indirect dispatch paths; internal arbitrary thread grids retain dispatchThreads.

Scope: Direct compute dispatch encoding. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan compute applications.

Implementation: [mtl_encoder.h](../src/kosmickrisp/bridge/mtl_encoder.h), [mtl_encoder.m](../src/kosmickrisp/bridge/mtl_encoder.m), [kk_cmd_dispatch.c](../src/kosmickrisp/vulkan/kk_cmd_dispatch.c).

### Share a lexical autorelease pool during command replay

Vulkan contract: Share a lexical autorelease pool during command replay.

Selected bridge calls share one lexical pool during synchronous command replay. A thread-local dynamic-extent marker restores on normal/exceptional exit; standalone calls and other threads retain local lexical fallback pools. Completion callback pools and retained native object ownership remain unchanged.

Scope: EndCommandBuffer replay, Submission re-recording, Encoder and render-state bridge calls. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan command recording and submission replay.

Implementation: [mtl_replay_autorelease.h](../src/kosmickrisp/bridge/mtl_replay_autorelease.h), [mtl_bridge.h](../src/kosmickrisp/bridge/mtl_bridge.h), [mtl_bridge.m](../src/kosmickrisp/bridge/mtl_bridge.m), [mtl_bridge.c](../src/kosmickrisp/bridge/stubs/mtl_bridge.c), [mtl_encoder.m](../src/kosmickrisp/bridge/mtl_encoder.m), [mtl_render_state.m](../src/kosmickrisp/bridge/mtl_render_state.m), [mtl_argument_table.m](../src/kosmickrisp/bridge/mtl_argument_table.m), [kk_cmd_buffer.c](../src/kosmickrisp/vulkan/kk_cmd_buffer.c), [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c).

### Bind changed dynamic depth stencil state once

Vulkan contract: Bind changed dynamic depth stencil state once.

When pipeline and dynamic depth/stencil state change together, bind the selected depth/stencil state once after the pipeline. Preserve depth-only changes, null states, pipeline-failure binding and existing object ownership.

Scope: Graphics pipeline and dynamic depth/stencil flush. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan dynamic depth/stencil graphics state.

Implementation: [kk_cmd_draw.c](../src/kosmickrisp/vulkan/kk_cmd_draw.c).

### Omit the already retired replay pool walk

Vulkan contract: Omit the already retired replay pool walk.

Keep individual command-buffer reset before free-list return and completion accounting. Omit the subsequent zero-flag whole-pool traversal once every private replay command buffer is already initial.

Scope: Replay command-buffer completion. Device support and operation restrictions are defined by the linked implementation.

Consumers: Vulkan submission replay.

Implementation: [kk_queue.c](../src/kosmickrisp/vulkan/kk_queue.c).

## Detailed specifications

* [ATOMIC INT64](reference/KOSMICKRISP_ATOMIC_INT64.md)
* [ATTACHMENT ACCESS](reference/KOSMICKRISP_ATTACHMENT_ACCESS.md)
* [CLIP DISTANCES](reference/KOSMICKRISP_CLIP_DISTANCES.md)
* [COMPILE OPTIONS](reference/KOSMICKRISP_COMPILE_OPTIONS.md)
* [COMPUTE DERIVATIVES](reference/KOSMICKRISP_COMPUTE_DERIVATIVES.md)
* [CONSERVATIVE RASTERIZATION](reference/KOSMICKRISP_CONSERVATIVE_RASTERIZATION.md)
* [CUBE FILTERING](reference/KOSMICKRISP_CUBE_FILTERING.md)
* [CULL DISTANCE](reference/KOSMICKRISP_CULL_DISTANCE.md)
* [DEPTH BIAS](reference/KOSMICKRISP_DEPTH_BIAS.md)
* [DEPTH STENCIL STATE](reference/KOSMICKRISP_DEPTH_STENCIL_STATE.md)
* [DEVICE ADDRESS COMMANDS](reference/KOSMICKRISP_DEVICE_ADDRESS_COMMANDS.md)
* [DEVICE GENERATED COMMANDS](reference/KOSMICKRISP_DEVICE_GENERATED_COMMANDS.md)
* [EXTENDED FLAGS](reference/KOSMICKRISP_EXTENDED_FLAGS.md)
* [EXTERNAL MEMORY](reference/KOSMICKRISP_EXTERNAL_MEMORY.md)
* [FORMATS](reference/KOSMICKRISP_FORMATS.md)
* [FP64](reference/KOSMICKRISP_FP64.md)
* [GEOMETRY BOUNDS](reference/KOSMICKRISP_GEOMETRY_BOUNDS.md)
* [GRAPHICS STAGES](reference/KOSMICKRISP_GRAPHICS_STAGES.md)
* [HELPER WRITES](reference/KOSMICKRISP_HELPER_WRITES.md)
* [IMAGE ALIASING](reference/KOSMICKRISP_IMAGE_ALIASING.md)
* [IMAGE ATOMIC INT64](reference/KOSMICKRISP_IMAGE_ATOMIC_INT64.md)
* [IMAGE VIEWS](reference/KOSMICKRISP_IMAGE_VIEWS.md)
* [INDIRECT TESSELLATION](reference/KOSMICKRISP_INDIRECT_TESSELLATION.md)
* [INTERLOCK](reference/KOSMICKRISP_INTERLOCK.md)
* [LEGACY RASTER](reference/KOSMICKRISP_LEGACY_RASTER.md)
* [LONG VECTORS](reference/KOSMICKRISP_LONG_VECTORS.md)
* [MATRIX TILES](reference/KOSMICKRISP_MATRIX_TILES.md)
* [MESH SHADERS](reference/KOSMICKRISP_MESH_SHADERS.md)
* [MSRTSS](reference/KOSMICKRISP_MSRTSS.md)
* [MULTISAMPLE](reference/KOSMICKRISP_MULTISAMPLE.md)
* [POLY LAYER REPLAY](reference/KOSMICKRISP_POLY_LAYER_REPLAY.md)
* [QUERIES](reference/KOSMICKRISP_QUERIES.md)
* [QUEUES](reference/KOSMICKRISP_QUEUES.md)
* [REQUIRED SUBGROUP SIZE](reference/KOSMICKRISP_REQUIRED_SUBGROUP_SIZE.md)
* [SAMPLERS](reference/KOSMICKRISP_SAMPLERS.md)
* [SAMPLER MINMAX](reference/KOSMICKRISP_SAMPLER_MINMAX.md)
* [SHADER CACHE](reference/KOSMICKRISP_SHADER_CACHE.md)
* [SPARSE](reference/KOSMICKRISP_SPARSE.md)
* [TRANSIENT ATTACHMENTS](reference/KOSMICKRISP_TRANSIENT_ATTACHMENTS.md)
* [VERTEX INPUT](reference/KOSMICKRISP_VERTEX_INPUT.md)
* [WRITABLE MSAA](reference/KOSMICKRISP_WRITABLE_MSAA.md)
* [barycentric](reference/kosmickrisp-barycentric.md)
* [descriptor heaps](reference/kosmickrisp-descriptor-heaps.md)
* [descriptor layout](reference/kosmickrisp-descriptor-layout.md)
* [independent interfaces](reference/kosmickrisp-independent-interfaces.md)
* [memory](reference/kosmickrisp-memory.md)
* [mesh pipeline api](reference/kosmickrisp-mesh-pipeline-api.md)
* [retained raster](reference/kosmickrisp-retained-raster.md)
* [sampler parameters](reference/kosmickrisp-sampler-parameters.md)
* [shader cache](reference/kosmickrisp-shader-cache.md)
* [varyings](reference/kosmickrisp-varyings.md)
* [zero initialization](reference/kosmickrisp-zero-initialization.md)
