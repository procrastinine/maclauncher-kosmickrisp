# Shader compile options

MacLauncher KosmicKrisp passes shader compilation context through `kk_compile_options`. A null options pointer selects zero defaults. The fields are:

| Field | Value |
| --- | --- |
| `per_vertex_map` | Optional shared producer/consumer transport map. |
| `independent_interface` | Independent-stage compilation context. |
| `mesh_queries` | Mesh query instrumentation option. |
| `msl_custom` | Optional native MSL translation options. |

`kk_compile_shader` consumes this structure in place of a separate transport-map argument. The per-vertex path supplies its map through the structure; calls with no transport map pass null.

`kk_compile_msl_options` copies the optional MSL settings, applies the driver memory context and workarounds, and supplies the mesh query option. The ordinary `kk_compile_msl` entry selects null defaults. Shader stage lowering, render-target component mapping, native source emission and shader ownership use the common compiler path.

Driver-owned buffer entries are declared in `src/kosmickrisp/libkk/kk_argument_table.h`: root0, samplers1, per-draw2, VS VFT wrapper3, FS VFT wrapper4, mesh parameters5 and interlock guard6. Production code uses the named constants. `kk_argument_table_buffer_count` takes a `kk_argument_table_usage` value and returns the highest used entry plus one. Linked graphics, mesh and interlock use the corresponding fields; an active guard requires seven entries. VFT entries hold addresses of wrapper buffers containing table resource IDs.

The slot constants are plain preprocessor definitions shared with Metal source. C-only usage and capacity declarations are excluded under `__METAL_VERSION__`. `KK_ARGUMENT_TABLE_MSL_PROLOG` expands to an MSL preprocessor prolog whose values are stringified from those same definitions, for runtime-compiled source without a second numeric table.
