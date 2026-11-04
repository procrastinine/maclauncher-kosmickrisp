# Subgroup matrices and FP8

KosmicKrisp exposes `VK_KHR_cooperative_matrix` for compute shaders on Apple GPU family 7 and later. A subgroup has 32 invocations. The supported multiplication properties use M=N=K=16 and subgroup scope:

| A and B components | C components | Result components | Saturating accumulation |
| --- | --- | --- | --- |
| FP16 | FP16 | FP16 | false |
| FP16 | FP32 | FP32 | false |
| FP8 E4M3 | FP32 | FP32 | false |
| FP8 E4M3 | FP8 E4M3 | FP32 | false |

The `cooperative-matrix-tile` adaptation represents a 16×16 matrix as four native 8×8 Metal SIMD-group fragments. Multiplication uses eight native 8×8 multiply-add operations. Each invocation holds eight opaque component values; Metal determines their distribution. Constructing, copying, converting, component arithmetic and per-invocation element access use the shared NIR scalar/vector machinery. Wide phi registers are split into native two-element fragments.

Device and workgroup-memory loads and stores preserve row-major and column-major layouts. Stride is measured in units of the original pointer's pointee type, including byte-backed and vector-backed memory. Buffer addresses use the shared explicit-I/O and descriptor lowering; workgroup addresses use its byte-offset route.

FP8 loads and stores discover native fragment coordinates by loading coordinate values through Metal's matrix operation. The coordinate data occupies existing compute root storage. This avoids a fixed SIMD lane map, hidden threadgroup staging memory, a separate dispatch and an additional buffer allocation. FP8 A/B operands are decoded to FP16 for native multiplication; an FP8 C operand is decoded to FP32 for accumulation. FP32 accumulation and results retain their component type.

`VK_EXT_shader_float8` supplies E4M3 and E5M2 scalar conversions on the GPU. Conversion preserves signed zero and subnormal values, rounds to nearest with ties to even, and handles NaNs and overflow. Unsaturated E4M3 overflow produces NaN; unsaturated E5M2 overflow produces infinity. Saturated conversion clamps finite overflow and infinity to the largest normal value with the input sign: 448 for E4M3 and 57344 for E5M2. FP8 matrix properties cover E4M3; E5M2 matrix multiplication is not enumerated.

`cooperativeMatrixRobustBufferAccess`, workgroup-scope cooperative matrices, integer matrix multiplication and the optional NV cooperative-matrix2 operations are not exposed. The compiler retains native 8×8 FP16/FP32 operations independently of the enumerated 16×16 Vulkan properties. D3D feature queries remain the responsibility of the consuming translation layer.
