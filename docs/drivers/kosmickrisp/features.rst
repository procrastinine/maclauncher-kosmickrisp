Features and Apple GPU families
*******************************

The table lists every capability that KosmicKrisp advertises and how the
driver provides it: natively through Metal, by emulation in shaders or compute
passes, or natively with some cases emulated. Rows that name an entry are
controlled by ``KK_EMULATION``; the emulation registry table below lists the cost, default and
reason for each entry. In the performance mode column, ``On`` and ``Off`` give
the entry's default in the default ``performance`` mode, ``Always`` marks a
capability advertised in every mode, and ``Experimental`` marks one that only
``MESA_KK_EXPERIMENTAL`` advertises. The native-on column first names the
earliest Apple GPU family for which Apple's `Metal Feature Set Tables
<https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>`__ (October
23, 2026) list the Metal feature, or "none listed" when the tables have no
matching entry. It then states on which families KosmicKrisp takes its native
path and on which it emulates the feature. KosmicKrisp runs only on Metal 4
devices, which are Apple7 and later, so "all families" means Apple7 and later.
The family-to-GPU mapping appears above. The last column gives the
recorded effect on Direct3D translation through DXVK, vkd3d-proton or Wine
vkd3d when an entry is disabled.

Feature table
=============

.. list-table::
   :header-rows: 1
   :widths: 20 26 22 10 8 14

   * - Feature
     - Implementation
     - Native on
     - KK_EMULATION entry
     - Performance mode
     - Direct3D effect when off
   * - ``geometryShader``, ``shaderTessellationAndGeometryPointSize``
     - Native object/mesh for bounded raster-only linked vertex and geometry
       shaders. Other geometry draws use compute and draw from memory.
     - Apple7 for the bounded object/mesh route.
     - —
     - Always
     - —
   * - ``tessellationShader``, ``extendedDynamicState2PatchControlPoints``,
       ``extendedDynamicState3TessellationDomainOrigin``
     - Emulated: vertex and control stages run as compute passes, and a
       compute tessellator generates the primitives. See
       `Tessellation`_.
     - Apple3, Metal 3 programming model only; emulated on all families.
     - —
     - Always
     - —
   * - ``multiviewGeometryShader``, ``multiviewTessellationShader``
     - Emulated: geometry and tessellation producers run once for each view.
     - None listed; emulated on all families.
     - ``multiview_geometry_tessellation``
     - On
     - vkd3d-proton reports view instancing as not supported, because its
       tier 2 check requires these features on KosmicKrisp.
   * - ``VK_EXT_mesh_shader``
     - Native with emulated cases: object and mesh shaders run natively.
       Task grids above the native bound run as compute batches, and
       oversized outputs are sliced. Apple7 and Apple8 batch native indirect
       inputs before complete rendering scopes.
     - Apple7 for object and mesh shading. Apple9 for mesh draws in indirect
       command buffers. The Metal 4 encoder-level indirect path also runs
       on M2.
     - ``mesh_shader``
     - On
     - D3D12 mesh shader tier 1 and feature level 12_2 are unavailable.
   * - ``VK_EXT_transform_feedback`` (``transformFeedback``,
       ``geometryStreams``)
     - Emulated: a compute geometry pass writes the captured vertices to the
       feedback buffers and counters.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_primitives_generated_query``
     - Emulated: shader counters and report kernels.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``pipelineStatisticsQuery``
     - Emulated: counters come from draw data and shader atomics. Shaders
       compile without counter code while no query is active.
     - None listed; emulated on all families.
     - ``pipeline_statistics``
     - On
     - Wine vkd3d is limited to feature level 11_0. DXVK treats the feature as
       optional.
   * - ``VK_EXT_conservative_rasterization``,
       ``extendedDynamicState3ConservativeRasterizationMode``,
       ``extendedDynamicState3ExtraPrimitiveOverestimationSize``
     - Emulated: triangles are expanded in a compute pass, and a fragment
       prologue computes coverage. Underestimation and conservative points
       and lines are not provided.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``fillModeNonSolid``, ``extendedDynamicState3PolygonMode``
     - Native with emulated cases: line mode uses native wireframe. Point mode
       expands captured primitives into two triangles per point; Mesh shader
       triangles draw native points at their vertices, and gl_FrontFacing of
       those points is the triangle's facing.
     - None listed; line mode native on all families.
     - ``polygon_points``
     - On
     - Direct3D translators lose non-solid fill, including wireframe,
       because Vulkan uses one feature bit for both modes.
   * - ``VK_KHR_line_rasterization``, ``VK_EXT_line_rasterization``
       (``wideLines``, ``rectangularLines``, ``smoothLines``,
       ``stippledBresenhamLines``, ``bresenhamLines``),
       ``extendedDynamicState3LineRasterizationMode``,
       ``extendedDynamicState3LineStippleEnable``
     - Native with emulated cases: thin solid lines are native. Thin lines in
       the default mode are drawn 1/256 pixel lower, so an endpoint on a pixel
       row boundary belongs to the row below it. Wide, smooth and stippled
       lines are expanded into triangles with coverage and dash generation.
       Stippled rectangular and stippled smooth lines are not provided.
     - None listed; thin lines native on all families.
     - ``line_expansion``
     - On
     - Wide, rectangular, smooth and stippled lines are hidden; thin lines
       remain.
   * - ``largePoints``
     - Native, up to 511 pixels.
     - Apple2 for 511-pixel points; native on all families.
     - —
     - Always
     - —
   * - ``shaderClipDistance``
     - Native.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``shaderCullDistance``
     - Native with emulated cases: mesh shaders reject primitives natively.
       Other final pre-rasterization stages that write cull distances are
       captured and tested before clipping.
     - None listed; mesh path native on all families.
     - ``cull_distance``
     - On
     - DXVK cannot create its D3D device.
   * - ``depthClamp``, ``depthBiasClamp``, ``VK_EXT_depth_clip_enable``,
       ``extendedDynamicState3DepthClampEnable``,
       ``extendedDynamicState3DepthClipEnable``
     - Native with emulated cases: Metal depth clip mode and depth bias state
       cover clip or clamp alone. With both enabled, the fragment shader
       clamps depth; with both disabled, the vertex shader applies the
       viewport depth transform.
     - None listed; single modes native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_depth_clip_control``,
       ``extendedDynamicState3DepthClipNegativeOneToOne``
     - Emulated: shaders remap clip-space depth from the [-1, 1] range.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_depth_bias_control``
     - Native FORMAT bias, including exact bias, uses Metal state on D16 and
       D32 attachments. FORCE_UNORM and FLOAT representations capture geometry
       and apply the bias after clipping.
     - Apple7 and later for FORMAT bias.
     - ``depth_bias_control``, ``depth_bias_force_unorm``, ``depth_bias_float``
     - FORMAT On; FORCE_UNORM and FLOAT Off
     - Exact FORMAT controls have no geometry replay cost. Alternate
       representations require their individual emulation entries.
   * - ``depthBounds``
     - Native with emulated cases: the Metal depth bounds test on Apple10 and
       later, in every mode. On earlier families, each draw of a pipeline
       that enables the test runs in the per-sample depth/stencil
       transaction, which tests the stored depth and discards samples
       outside the bounds without changing stencil. A pipeline with a
       dynamic enable draws through its unchanged pipeline while the test
       is off.
     - Apple10; native on Apple10 and later, emulated on Apple7 to Apple9.
     - ``depth_bounds``
     - Off
     - On Apple7 to Apple9, vkd3d-proton reports DepthBoundsTestSupported as
       false, which limits Direct3D 12 to feature level 12_1.
   * - ``VK_EXT_provoking_vertex``,
       ``extendedDynamicState3ProvokingVertexMode``
     - Native with emulated cases: when the last vertex is provoking and the
       fragment shader has flat inputs, a compute pass reorders the vertices.
       A Mesh pipeline with a static last-vertex mode writes each triangle's
       and line's indices with the last vertex first; with a dynamic mode it
       selects that order or the original one from the mode set for the draw.
     - None listed; first-vertex mode native on all families.
     - —
     - Always
     - —
   * - ``fullDrawIndexUint32``, ``VK_KHR_index_type_uint8``,
       ``VK_EXT_index_type_uint8``,
       ``VK_EXT_primitive_restart_index``,
       ``VK_EXT_primitive_topology_list_restart``, triangle fans
     - Native with emulated cases: 32-bit and 16-bit indices with default
       strip restart are native. A direct, non-indexed triangle fan of up to
       4096 triangles is drawn as a triangle list with indices written on the
       CPU. 8-bit indices, other triangle fans, list restart and other restart
       indices are rewritten by a compute pass. Patch-list restart is not
       provided.
     - None listed; emulated cases on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_conditional_rendering``
     - Emulated: a compute pass copies or zeroes the indirect arguments of
       predicated draws and dispatches.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``drawIndirectFirstInstance``, ``multiDrawIndirect``,
       ``VK_KHR_draw_indirect_count``, ``VK_EXT_multi_draw``,
       ``VK_KHR_device_address_commands``, ``VK_KHR_shader_draw_parameters``
     - Native with emulated cases: indirect draws are native. Draw counts read
       from a buffer are applied by a compute predicate.
     - Apple3 for indirect arguments and base vertex and instance; native on
       all families.
     - —
     - Always
     - —
   * - ``VK_EXT_sample_locations``,
       ``extendedDynamicState3SampleLocationsEnable``
     - Native: Metal programmable sample positions. Single-sample rendering
       has no custom locations.
     - Apple2; native on all families.
     - —
     - Always
     - —
   * - ``sampleRateShading``, ``variableMultisampleRate``, ``alphaToOne``,
       ``extendedDynamicState3RasterizationSamples``
     - Native with emulated cases: alpha-to-one is applied in the fragment
       shader.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``extendedDynamicState3SampleMask``,
       ``extendedDynamicState3AlphaToCoverageEnable``
     - Native with emulated cases: static coverage state is native. Dynamic
       sample masks and alpha-to-coverage are applied in the fragment shader.
     - None listed; static state native on all families.
     - ``dynamic_coverage``
     - On
     - Dynamic sample-mask and alpha-to-coverage selectors are hidden; static
       coverage remains.
   * - ``VK_EXT_multisampled_render_to_single_sampled``
     - Emulated: a multisampled intermediate image is loaded from and resolved
       into the single-sampled attachment.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``shaderStorageImageMultisample``, storage sample counts 1, 2 and 4
     - Emulated: each sample is stored as a separate single-sample texture.
       Attachment contents are loaded and published at render-pass
       boundaries.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_depth_stencil_resolve``, ``VK_EXT_custom_resolve``
     - Native with emulated cases: color average, depth sample-zero, minimum
       and maximum, and stencil sample-zero resolve in the render pass. Depth
       average and stencil minimum and maximum use a shader resolve pass.
       Custom resolve starts a pass in which the application draws the
       resolve.
     - Apple3 for depth resolve, Apple5 for stencil resolve; native on all
       families.
     - —
     - Always
     - —
   * - ``VK_EXT_fragment_shader_interlock``:
       ``fragmentShaderSampleInterlock``
     - Native: raster order groups.
     - Apple4; native on all families.
     - —
     - Always
     - —
   * - ``fragmentShaderPixelInterlock``
     - Native with emulated cases: single-sample pixel interlock uses raster
       order groups. Multisampled pixel interlock replays fragments in ordered
       load, replay and store passes.
     - Apple4 for raster order groups; single-sample native on all families.
     - ``pixel_interlock``
     - On
     - vkd3d-proton loses rasterizer-ordered views and feature level 12_1.
   * - ``VK_EXT_rasterization_order_attachment_access``
       (``rasterizationOrderColorAttachmentAccess``)
     - Native: ordered color attachment reads. Ordered depth and stencil
       access is not provided.
     - Apple2 for programmable blending, Apple4 for raster order groups;
       native on all families.
     - —
     - Always
     - —
   * - ``independentBlend``, ``dualSrcBlend``, ``logicOp``,
       ``VK_EXT_blend_operation_advanced``
     - Native: all blend equations, including logic operations and advanced
       modes, run in the fragment shader on programmable-blending reads.
       Dynamic logic operations are not provided.
     - Apple2; native on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_dynamic_rendering_local_read``,
       ``VK_EXT_attachment_feedback_loop_layout``,
       ``VK_EXT_attachment_feedback_loop_dynamic_state``, input attachments
     - Native: color inputs read the framebuffer; depth and stencil inputs
       read the attachment image.
     - Apple2 for programmable blending; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_post_depth_coverage``
     - Native.
     - Apple4; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_shader_stencil_export``
     - Native.
     - Apple5 for stencil feedback; native on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_multiview``
     - Native: vertex amplification and layered rendering, up to 32 views.
     - Apple6 for vertex amplification; native on all families.
     - —
     - Always
     - —
   * - ``multiViewport``, ``shaderOutputViewportIndex``,
       ``shaderOutputLayer``, ``VK_EXT_shader_viewport_index_layer``
     - Native.
     - Apple5 for multiple viewports and layered rendering; native on all
       families.
     - —
     - Always
     - —
   * - ``VK_KHR_fragment_shader_barycentric``
     - Native coordinates with shader-local corner transport for exact or
       reordered coordinates and per-vertex inputs. Ordinary interpolation
       is unchanged when the shader does not request barycentric inputs.
     - Apple7 for coordinates; Apple10 for eligible native pre-raster
       per-vertex values. Other per-vertex reads use corner transport.
     - ``fragment_barycentric``
     - On Apple7-11
     - SV_Barycentrics shaders are unavailable. vkd3d-proton disables mesh
       shaders, so D3D12 mesh shader tier 1 and feature level 12_2 are
       unavailable.
   * - ``VK_KHR_fragment_shading_rate``
     - Emulated: pipeline, primitive and attachment rates up to 2x2 pixels.
       Each Metal quad or half quad forms one fragment. One covered lane per
       fragment runs the shader body and its outputs are broadcast to the
       other pixels; the other lanes evaluate neighboring fragments for
       derivatives. A multisampled shader that writes memory, which Metal
       runs once per covered sample, selects one lane across the samples of
       each fragment and primitive. Attachment texels are 8x8. Sample shading, shader depth,
       stencil and sample mask writes, sample mask reads, interlock, custom
       sample locations, conservative rasterization and a partial static or
       dynamic sample mask select 1x1. Primitive rates come from the last
       vertex, tessellation evaluation, geometry or mesh stage; a stage that
       does not write one provides 1x1.
     - No Apple family.
     - ``fragment_shading_rate``
     - Off
     - vkd3d-proton reports variable shading rate tier 0, which keeps
       Direct3D 12 at feature level 12_1.
   * - ``VK_KHR_shader_quad_control``
     - Native with emulated cases: quad operations are native. Full and
       coarse quad helper requirements add a triangle-merge guard.
     - Apple4 for quad permute, Apple7 for quad reductions; native on all
       families.
     - ``quad_control``
     - On
     - vkd3d-proton exposes at most shader model 6.6.
   * - ``VK_KHR_compute_shader_derivatives``
     - Emulated: derivatives use SIMD-group shuffles over four consecutive
       lanes. Only linear derivative groups are provided.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - Subgroup operations, ``VK_EXT_subgroup_size_control``,
       ``computeFullSubgroups``,
       ``VK_KHR_shader_subgroup_extended_types``,
       ``VK_KHR_shader_subgroup_rotate``,
       ``VK_KHR_shader_subgroup_uniform_control_flow``,
       ``VK_KHR_shader_maximal_reconvergence``,
       ``VK_EXT_shader_subgroup_ballot``, ``VK_EXT_shader_subgroup_vote``
     - Native: 32-wide SIMD groups.
     - Apple6 for SIMD permute, Apple7 for SIMD and quad reductions; native
       on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_shader_demote_to_helper_invocation``,
       ``VK_KHR_shader_terminate_invocation``
     - Native.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``shaderDenormPreserveFloat16``, ``shaderDenormPreserveFloat32``,
       ``shaderDenormPreserveFloat64``
     - Native with emulated cases: FP16 preservation uses native half
       arithmetic. In shaders that request FP32 preservation, FP32 arithmetic
       and comparisons run in software, and FP64 uses software binary64.
     - Apple11 for FP32 denormals; FP32 emulated on all families.
     - ``denorm_preserve``
     - On
     - vkd3d-proton limits non-NVIDIA D3D12 devices to shader model 6.1.
   * - ``shaderDenormFlushToZeroFloat32``
     - Emulated: native arithmetic plus exponent-mask fixups after selected
       operations. FP16 and FP64 flush-to-zero are not provided.
     - None listed; emulated on all families.
     - ``denorm_flush``
     - On
     - vkd3d-proton limits non-NVIDIA D3D12 devices to shader model 6.1.
   * - ``VK_KHR_shader_float_controls``, ``VK_KHR_shader_float_controls2``:
       round-to-nearest-even and signed zero, infinity and NaN preservation
     - Native with emulated cases: FP16 and FP32 modes use per-operation
       Metal math-mode pragmas. FP64 modes come from the software binary64
       implementation. Round-toward-zero is not provided.
     - None listed; FP16 and FP32 native on all families.
     - —
     - Always
     - —
   * - ``shaderFloat64``, ``shaderFmaFloat64``
     - Emulated: software binary64.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``shaderInt64``
     - Native.
     - Apple3; native on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_shader_atomic_int64`` (``shaderBufferInt64Atomics``,
       ``shaderSharedInt64Atomics``)
     - Full-width transactions use a hashed lock. Apple8 and later use native
       assistance for unsigned buffer minimum and maximum with unused results;
       other buffer and workgroup operations use two 32-bit words.
     - Apple9 hardware atomic capability; the public MSL path retains the
       emulated transaction on all families. Apple8 macOS provides unsigned
       buffer minimum and maximum assistance.
     - ``atomic64``
     - On
     - vkd3d-proton cannot advance from SM 6.5 to SM 6.6 and loses its shared
       int64 atomic capability. Ordinary ``shaderInt64`` remains independent.
   * - ``VK_EXT_shader_image_atomic_int64``, 64-bit integer storage images and
       texel buffers
     - Emulated: each operation takes a hashed lock. On Apple8 and later a
       native unsigned 64-bit texture minimum or maximum writes the result.
       On Apple7 the locked operation reads and writes the two 32-bit words
       of the texel.
     - Apple9 for all 64-bit atomics, Apple8 on macOS for unsigned minimum
       and maximum; emulated on all families.
     - ``image_atomic64``
     - Always on Apple8 and later; Off on Apple7
     - On Apple7, vkd3d-proton reports sampler feedback tier 0 and no
       typed-resource 64-bit atomics, so feature level 12_2 is unavailable.
   * - ``VK_EXT_shader_atomic_float``
     - Native: 32-bit float buffer and workgroup atomics. Workgroup atomic add
       requires macOS 27.
     - Apple7; native on all families.
     - —
     - Always
     - —
   * - ``shaderFloat16``, ``shaderInt16``, ``shaderInt8``,
       ``VK_KHR_16bit_storage``, ``VK_KHR_8bit_storage``,
       ``VK_KHR_shader_float16_int8``
     - Native.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_shader_float8``
     - Emulated: FP8 values are converted to and from wider types around
       native arithmetic.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_cooperative_matrix``
     - Native: 16×16×16 matrices built from 8×8 SIMD-group matrix tiles, in
       compute shaders only. FP16 and FP32 types and FP8 E4M3 inputs are
       provided; robust matrix buffer access is not.
     - Apple7 for SIMD-group matrix multiply; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_shader_long_vector``
     - Emulated: vectors longer than four components become scalar arrays.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_shader_integer_dot_product``
     - Native: integer arithmetic. No accelerated packed forms are reported.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - Shader language features: ``VK_KHR_spirv_1_4``,
       ``VK_KHR_vulkan_memory_model``, ``VK_KHR_variable_pointers``,
       ``VK_KHR_shader_untyped_pointers``,
       ``VK_KHR_workgroup_memory_explicit_layout``,
       ``VK_EXT_scalar_block_layout``,
       ``VK_KHR_uniform_buffer_standard_layout``,
       ``VK_KHR_relaxed_block_layout``,
       ``VK_KHR_storage_buffer_storage_class``,
       ``VK_KHR_shader_expect_assume``,
       ``VK_KHR_shader_relaxed_extended_instruction``,
       ``VK_KHR_shader_non_semantic_info``,
       ``VK_EXT_shader_replicated_composites``, ``VK_KHR_shader_fma``,
       ``VK_GOOGLE_decorate_string``, ``VK_GOOGLE_hlsl_functionality1``,
       ``VK_GOOGLE_user_type``, ``VK_AMD_shader_image_load_store_lod``,
       ``shaderStorageImageExtendedFormats``,
       ``shaderStorageImageReadWithoutFormat``,
       ``shaderStorageImageWriteWithoutFormat``,
       ``fragmentStoresAndAtomics``, ``vertexPipelineStoresAndAtomics``
     - Native: translated to Metal Shading Language. 64-bit FMA uses the
       software binary64 implementation.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_zero_initialize_workgroup_memory``
     - Emulated: shaders that request it clear their workgroup memory with
       stores followed by a barrier.
     - None listed; emulated on all families.
     - ``workgroup_zero``
     - On
     - DXVK and vkd3d device creation fails.
   * - ``VK_EXT_robustness2``, ``VK_KHR_robustness2``
       (``robustBufferAccess2``, ``robustImageAccess2``, ``nullDescriptor``)
     - Emulated: selected buffer, image and null-descriptor accesses use
       bounds checks and default values in the shader.
     - None listed; emulated on all families.
     - ``robustness2``
     - On
     - DXVK and vkd3d-proton device creation fails.
   * - ``robustBufferAccess``, ``VK_EXT_image_robustness``,
       ``VK_EXT_pipeline_robustness``, ``VK_EXT_vertex_attribute_robustness``
     - Native with emulated cases: buffer and vertex-input accesses are
       bounds-checked in the shader. On Apple10, out-of-range LOD image reads
       add a shader check (:doc:`workaround 16 <kosmickrisp/workarounds>`).
     - None listed; buffer checks emulated on all families.
     - —
     - Always
     - —
   * - Vertex input, ``VK_KHR_vertex_attribute_divisor``,
       ``VK_EXT_vertex_attribute_divisor``
     - Native: vertex shaders fetch attributes from buffer addresses, with
       static layouts compiled in. Zero divisors are provided.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_vertex_input_dynamic_state``
     - Emulated: vertex shaders load the attribute format, stride and divisor
       at run time and convert the data.
     - None listed; emulated on all families.
     - ``dynamic_vertex_input``
     - Off
     - None; DXVK and vkd3d-proton do not require it for a shader model,
       feature level or device creation.
   * - ``VK_EXT_extended_dynamic_state``, ``VK_EXT_extended_dynamic_state2``,
       ``VK_EXT_extended_dynamic_state3`` (fields not listed in other rows),
       ``VK_KHR_dynamic_rendering``,
       ``VK_EXT_dynamic_rendering_unused_attachments``
     - Native: Metal render state or pipeline variants. Unused attachment
       outputs are specialized out of the fragment shader.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - Descriptor indexing and update-after-bind,
       ``VK_EXT_descriptor_indexing``, ``VK_KHR_push_descriptor``,
       ``VK_EXT_inline_uniform_block``, ``VK_EXT_mutable_descriptor_type``,
       ``VK_KHR_descriptor_update_template``
     - Native: argument buffers.
     - Apple6 for argument buffers tier 2; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_descriptor_heap``
     - Native: heap descriptors hold GPU addresses and Metal resource IDs.
       Capture and replay is not provided.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``VK_KHR_buffer_device_address``, ``VK_EXT_buffer_device_address``
     - Native: Metal GPU addresses.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_texel_buffer_alignment``
       (``storageTexelBufferOffsetSingleTexelAlignment``,
       ``uniformTexelBufferOffsetSingleTexelAlignment``)
     - Emulated: views at single-texel offsets read aligned native textures
       or raw buffer addresses with an adjusted coordinate and range.
     - None listed; emulated on all families.
     - ``texel_buffer_alignment``
     - On
     - vkd3d-proton rejects D3D12 device creation.
   * - ``VK_EXT_device_generated_commands``
     - Native: compute passes preprocess the commands into Metal indirect
       command buffers for draws and dispatches. Mesh and task stages, pipeline
       switching and transform feedback are not provided.
     - Apple3 for indirect command buffers, Apple9 for mesh draws in them;
       native on all families, without mesh stages.
     - —
     - Always
     - —
   * - ``samplerAnisotropy``, ``VK_KHR_sampler_mirror_clamp_to_edge``,
       ``imageCubeArray``, ``shaderResourceMinLod``,
       ``shaderImageGatherExtended``, LOD queries
     - Native with emulated cases: gathers with separate per-texel offsets
       are split into single gathers, and 1D images use 2D textures. LOD queries
       use Metal when ``supportsQueryTextureLOD`` is true; otherwise shaders
       calculate the gradient LOD and apply sampler and mip-level limits.
     - Apple2 for anisotropy, Apple4 for cube arrays, Apple8 for LOD queries
       (some Apple7 devices). LOD-query eligibility is cached per device.
     - —
     - Always
     - —
   * - Fixed sampler border colors
     - Native with emulated cases: transparent black, opaque white and integer
       opaque black are Metal border colors. Float opaque black, except with
       depth comparison, uses the shader border path, which corrects A8 and
       packed formats.
     - Apple7; native on all families.
     - —
     - Always
     - —
   * - ``mipLodBias``, ``maxSamplerLodBias``
     - Native with emulated cases: Metal samplers apply the bias on Apple10
       and later. On earlier families, the shader adds it.
     - Apple10; native on Apple10 and later, emulated on Apple7 to Apple9.
     - —
     - Always
     - —
   * - ``VK_EXT_sampler_filter_minmax``
     - Native with emulated cases: Metal sampler reduction on Apple10 and
       later. On earlier families, the shader reduces the filter footprint,
       which covers every sampleable format.
     - Apple10; native on Apple10 and later, emulated on Apple7 to Apple9.
     - —
     - Always
     - —
   * - ``VK_EXT_non_seamless_cube_map``
     - Emulated: non-seamless samples are projected to a 2D-array face sample
       with recomputed gradients. An independent function constant retains
       native sampling until a non-seamless sampler is requested.
     - None listed; emulated on all families.
     - ``non_seamless_cube``
     - On Apple7-11
     - DXVK rejects content that requests non-seamless cube sampling.
   * - ``VK_EXT_custom_border_color``, ``VK_EXT_border_color_swizzle``
     - Emulated: the shader substitutes the border color.
     - Apple7 lists fixed border colors only; emulated on all families.
     - —
     - Experimental
     - —
   * - ``VK_EXT_image_view_min_lod``
     - Emulated: the shader clamps the LOD to the view's minimum.
     - None listed; emulated on all families.
     - —
     - Experimental
     - —
   * - ``VK_KHR_sampler_ycbcr_conversion``,
       ``VK_EXT_ycbcr_2plane_444_formats``
     - Emulated: the shader samples each plane and converts the color model.
     - None listed; emulated on all families.
     - —
     - Always
     - —
   * - Compressed and packed formats: ``textureCompressionBC``,
       ``textureCompressionETC2``, ``textureCompressionASTC_LDR``,
       ``VK_EXT_texture_compression_astc_hdr``, ``VK_EXT_4444_formats``
     - Native: some packed formats use texture swizzles. BC capabilities and
       format reports follow the device's ``supportsBCTextureCompression``.
     - Apple2 for ASTC and ETC/EAC, Apple9 for BC (some Apple7 and Apple8
       devices); BC support is queried from the device.
     - —
     - Always
     - —
   * - ``R32G32B32`` formats
     - Native read-only texel buffers on Apple11 with macOS 27 when the
       aligned view permits whole RGB texels. Writable, sparse and other
       texel-buffer cases use a single-channel view three times as wide.
       Images use four-channel textures with a hidden fourth component;
       transfers keep the 12-byte layout through staging.
     - Apple11 for read-only RGB texture buffers; other cases remain emulated.
     - —
     - Always
     - —
   * - ``VK_EXT_image_2d_view_of_3d``
     - Native: a 2D-array texture over the 3D image's memory.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``VK_EXT_image_sliced_view_of_3d``
     - Native with emulated cases: storage views keep the native 3D texture,
       and the shader offsets and bounds-checks the slice range.
     - None listed; emulated slice addressing on all families.
     - —
     - Always
     - —
   * - ``blockTexelViewCompatibleMultipleLayers``, ``VK_KHR_maintenance11``
     - Native with emulated cases: block-compatible views of several layers
       use one native alias per layer, selected in the shader.
     - None listed; emulated layer selection on all families.
     - —
     - Always
     - —
   * - Image dimension limits
     - Native: 1D, 2D and cube images reach 16,384 pixels, and 32,768 pixels
       on Apple10 and later.
     - Apple10 for 32,768 pixels; native on Apple10 and later.
     - —
     - Always
     - —
   * - ``sparseBinding``, ``sparseResidencyImage2D``,
       ``sparseResidencyAliased``, ``shaderResourceResidency``,
       ``sparseResidencyStandard2DBlockShape``,
       ``sparseResidencyNonResidentStrict``
     - Native: Metal placement sparse with 64 KiB pages. Advertised when the
       device reports placement sparse with 64 KiB tiles; before macOS 26.4,
       Apple8 is also required.
     - Apple8, and Apple7 devices that report placement sparse, including
       all Apple7 Macs; native where advertised.
     - —
     - Always
     - —
   * - ``sparseResidencyBuffer``, ``sparseResidencyImage3D``,
       ``sparseResidencyStandard3DBlockShape``
     - Native with emulated cases: placement and mapping are native. Buffer
       residency checks and standard-shaped 3D volumes are translated in the
       shader.
     - Apple8 for placement sparse buffers; translation on all families
       where sparse is advertised.
     - ``sparse_translation``
     - On
     - DXVK tiled-resource tiers and feature levels that require sparse
       buffer or 3D residency are unavailable.
   * - ``VK_EXT_zero_initialize_device_memory``
     - Native: a CPU or GPU clear at allocation. Zero-initialized images use
       the uncompressed layout.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - Memory: ``VK_EXT_external_memory_host``,
       ``VK_KHR_external_memory_fd``, ``VK_EXT_external_memory_metal``,
       ``VK_EXT_map_memory_placed``, ``VK_EXT_memory_budget``,
       ``VK_EXT_memory_priority``, ``VK_EXT_pageable_device_local_memory``,
       ``VK_MESA_image_alignment_control``,
       ``VK_EXT_image_compression_control``, ``VK_EXT_host_image_copy``
     - Native. Memory priority uses two residency classes.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - ``occlusionQueryPrecise``, ``inheritedQueries``,
       ``VK_EXT_host_query_reset``, ``VK_KHR_calibrated_timestamps``,
       ``VK_EXT_calibrated_timestamps``
     - Native: counting visibility results and GPU timestamps.
     - Apple3 for counting occlusion queries; native on all families.
     - —
     - Always
     - —
   * - Timestamps written outside a render pass (``vkCmdWriteTimestamp2``)
     - Native Metal timestamps. Off: every timestamp in one compute encoder
       reports the encoder's first relaxed sample, with no GPU wait. On: each
       timestamp is precise and waits for the encoder's earlier dispatches,
       about 7 microseconds of GPU time per timestamp the application writes.
     - None listed; native on all families.
     - ``precise_timestamps``
     - Off
     - Direct3D timestamp queries issued within one compute encoder report
       one value, so GPU durations measured inside it are 0.
   * - ``VK_EXT_graphics_pipeline_library``, ``VK_KHR_pipeline_library``
     - Native stage bodies and final Metal render pipeline assembly for
       ordinary one-sample vertex/color fragment paths. Coupled state and
       other stage chains use retained canonical sources with the complete
       compiler. Independent interpolation is provided; fast linking is false.
     - Apple7 through Apple11; no fast-link threshold is declared.
     - ``graphics_pipeline_library``
     - On
     - Translator graphics pipeline library paths are unavailable.
   * - ``VK_KHR_acceleration_structure``, ``VK_KHR_ray_tracing_pipeline``,
       ``VK_KHR_ray_query``, ``VK_KHR_ray_tracing_maintenance1``,
       ``VK_EXT_pipeline_library_group_handles``
     - Emulated: acceleration structures are built and traversed in compute,
       and ray-tracing pipelines run as continuation dispatches. Metal ray
       tracing is not used. Shader group handles derive from the group's
       shaders, so a pipeline library and every pipeline linked from it
       report the same handle for a group.
     - Apple6 for Metal ray tracing, Apple9 for intersection result storage
       and intersection function buffers; emulated on all families.
     - ``ray_tracing``
     - Off
     - D3D12 DXR is unavailable.
   * - Commands, synchronization and queues: ``VK_KHR_synchronization2``,
       ``VK_KHR_timeline_semaphore``, ``VK_KHR_external_fence``,
       ``VK_KHR_external_fence_fd``, ``VK_KHR_external_semaphore``,
       ``VK_KHR_external_semaphore_fd``, ``VK_KHR_external_memory``,
       ``VK_KHR_global_priority``, ``VK_EXT_global_priority``,
       ``VK_EXT_global_priority_query``, ``VK_EXT_nested_command_buffer``,
       ``VK_AMD_buffer_marker``, ``VK_KHR_deferred_host_operations``
     - Native. One queue priority level is reported.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - API and maintenance extensions: ``VK_KHR_maintenance1`` to
       ``VK_KHR_maintenance10``, ``VK_KHR_bind_memory2``,
       ``VK_KHR_dedicated_allocation``,
       ``VK_KHR_get_memory_requirements2``, ``VK_KHR_device_group``,
       ``VK_KHR_driver_properties``, ``VK_KHR_copy_commands2``,
       ``VK_KHR_create_renderpass2``, ``VK_KHR_imageless_framebuffer``,
       ``VK_KHR_image_format_list``, ``VK_KHR_format_feature_flags2``,
       ``VK_KHR_separate_depth_stencil_layouts``,
       ``VK_EXT_separate_stencil_usage``, ``VK_KHR_unified_image_layouts``,
       ``VK_KHR_load_store_op_none``, ``VK_EXT_load_store_op_none``,
       ``VK_KHR_map_memory2``, ``VK_KHR_extended_flags``,
       ``VK_EXT_pipeline_creation_cache_control``,
       ``VK_EXT_pipeline_creation_feedback``, ``VK_EXT_private_data``,
       ``VK_EXT_tooling_info``, ``VK_EXT_debug_marker``
     - Native.
     - None listed; native on all families.
     - —
     - Always
     - —
   * - Presentation: ``VK_KHR_swapchain``,
       ``VK_KHR_swapchain_maintenance1``, ``VK_EXT_swapchain_maintenance1``,
       ``VK_KHR_swapchain_mutable_format``, ``VK_KHR_incremental_present``,
       ``VK_KHR_present_id``, ``VK_KHR_present_id2``,
       ``VK_KHR_present_wait``, ``VK_KHR_present_wait2``,
       ``VK_GOOGLE_display_timing``, ``VK_EXT_hdr_metadata``
     - Native. All but ``VK_EXT_hdr_metadata`` are advertised only in builds
       with the window-system platform.
     - None listed; native on all families.
     - —
     - Always
     - —

Capabilities without a Metal feature
====================================

Apple's tables list no Metal feature for the following capabilities.
KosmicKrisp emulates each of them:

- **Conservative rasterization**: triangles are expanded in a compute pass, and
  a fragment prologue computes coverage. Overestimation is provided;
  underestimation and conservative points and lines are not.
- **Transform feedback**: vertices are captured through the compute geometry
  path and written to the feedback buffers. Feedback queries and
  draw-from-feedback use small compute kernels.
- **Geometry shaders**: bounded direct draws with linked vertex and geometry
  shaders use native object and mesh stages on Apple7 and later families.
  One object group evaluates the input primitive's vertices, and one child
  mesh group executes each geometry invocation. Geometry strips become
  explicit point, line or triangle lists with preserved strip parity and
  provoking values. Declared output counts and the complete linked output
  footprint must fit 256 mesh vertices, 512 primitives, 16 KiB mesh output
  and 16 KiB object payload. Grid and aggregate output bounds also apply.
  Indirect draws, tessellation, adjacency, primitive restart, shader memory
  writes or atomics, subgroup-sensitive programs, active transform feedback,
  nonzero streams, geometry reports and retained geometry consumers use the
  existing compute path. These routes use the same geometry capability;
  internal object/mesh use is independent of the Vulkan mesh-shader controls.
- **Custom border colors**: Metal provides transparent black, opaque black and
  opaque white (Apple7). Other colors are substituted in the shader, and
  ``VK_EXT_custom_border_color`` and ``VK_EXT_border_color_swizzle`` are
  advertised only with ``MESA_KK_EXPERIMENTAL=custom_border``.
- **Non-seamless cube sampling**: the tables list no non-seamless cube mode.
  The ``non_seamless_cube`` entry projects such samples onto a single face and
  is on in ``performance`` mode. Descriptor-set pipelines without a
  non-seamless sampler retain native sampling through an independent function
  constant; border and reduction samplers do not activate cube projection.

Tessellation
============

Apple's tables list tessellation (Apple3) under the Metal 3 programming model
only; the Metal 4 programming model has no tessellation stage. Because
KosmicKrisp runs on Metal 4, it does not use Metal's tessellation API. Instead,
the vertex and tessellation control stages run as compute passes, and a
compute tessellator generates the tessellated primitives. These are then drawn
through the same path as geometry-shader output, on every family.
