# Conservative rasterization

MacLauncher KosmicKrisp implements triangle overestimation through `VK_EXT_conservative_rasterization`. The final pre-raster stage runs through the existing Poly primitive capture. Original polygons are clipped and culled before expansion. Expansion intersects the polygon with pixel-square coverage bounds and emits whole pixel-row spans, so coverage is established before depth and stencil tests.

The fragment stage reads the original three vertices through the shared per-vertex heap transport. Smooth attributes use perspective weights, linear attributes use screen-space weights, and flat attributes retain the original provoking value. Attributes and depth extrapolate outside the original triangle. Expansion preserves transform feedback, generated-primitive queries and original-stage side effects.

| Property | Value |
|---|---:|
| `primitiveOverestimationSize` | 1/256 pixel |
| `maxExtraPrimitiveOverestimationSize` | 0 |
| `extraPrimitiveOverestimationSizeGranularity` | 0 |
| `primitiveUnderestimation` | false |
| `conservativePointAndLineRasterization` | false |
| `degenerateTrianglesRasterized` | true |
| `degenerateLinesRasterized` | false |
| `fullyCoveredFragmentShaderInputVariable` | true |
| `conservativeRasterizationPostDepthCoverage` | false |

Triangles that collapse to a point or line on the raster grid retain their conservative coverage. They are backfacing, and their attributes and depth use the original provoking vertex. Point and line rasterization use their ordinary paths. Underestimation is unavailable. These properties satisfy conservative rasterization tier 3 in DXVK and vkd3d-proton; their feature-level reporting also depends on other device capabilities.

The conservative mode and zero extra size support extended dynamic state. Pipelines that declare a dynamic conservative mode compile the required raster variants in advance. Statically disabled pipelines retain their ordinary MSL and draw uploads. Conservative expansion allocates primitive packets and row spans only for enabled draws. Each covered row emits two triangles, and multisampled pixels retain their full geometric coverage before the application sample mask applies.

The conservative rasterization, degenerate triangle and inner coverage implementations are ordinary commits in the Mesa tree.

The fragment input sample mask is the original primitive's full geometric sample mask intersected with the API sample mask, restricted to the invocation sample when sample shading is enabled. It remains a pre-depth/stencil value with early fragment tests. A zero logical input mask prevents application fragment shader execution. Conservative pipelines do not support the PostDepthCoverage execution mode.

FullyCoveredEXT describes the entire logical fragment rectangle in the original clipped primitive. It is independent of the API sample mask, sample shading, native row triangulation and depth/stencil results. Degenerates and disabled conservative mode return false. The predicate uses the convex hull of clipped vertices on the 1/256-pixel raster grid, with a half-grid inward margin and signed 64-bit edge tests. Original attributes retain their existing interpolation.

Only enabled conservative draws whose fragment shader reads FullyCoveredEXT allocate inner-edge storage. Each clipped vertex reserves 24 bytes, with one packet pointer. Preparation builds the convex hull in the existing pass; fragment evaluation tests its edges. The callable fragment interface receives the Boolean through SYSTEM_VALUE_FULLY_COVERED. The conservative prolog also accepts explicit logical rectangle bounds for coarse-fragment integrations.
