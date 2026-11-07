# Binary64 shader arithmetic

KosmicKrisp supplies `shaderFloat64` through integer software arithmetic. The compiler lowers binary64 operations before MSL emission, using Mesa's NIR double lowering and its Berkeley SoftFloat-derived GLSL library. Metal receives integer arithmetic and the existing shader resource layout.

Binary64 arithmetic uses round-to-nearest-even with gradual underflow. Signed zero, infinities and NaNs retain their Vulkan semantics; NaN payloads are unspecified. Division retains the exact quotient remainder, and fused multiply-add rounds the complete product and addend once. Conversions to binary16 round directly without a binary32 intermediate. Integer conversions truncate toward zero within their defined range.

The float-control properties report denormal preservation, signed-zero/infinity/NaN preservation and round-to-nearest-even for binary64. Binary64 flush-to-zero and the round-toward-zero execution mode remain unavailable. Binary16 preservation and the selected binary32 preserve or flush mode operate independently, including conversions between widths. A conversion decorated for binary16 round-toward-zero uses that conversion's requested rounding mode.

Subgroup binary64 arithmetic and votes use the existing 32-invocation subgroup with software arithmetic and native bitwise shuffles. Resource loads, stores and bitcasts retain their original widths.

The library is loaded and inlined only while compiling a shader containing binary64 operations. Other shaders skip that lowering. Binary64 adds no descriptor, upload, allocation or per-draw driver operation; its arithmetic runs as integer shader instructions and therefore costs more than native hardware floating-point arithmetic.

The software library derives from Mesa commit `4f071f891d63396c799b267172c19ba6b0301682`, including `src/compiler/glsl/float64.glsl` and `nir_lower_double_ops.c`. Berkeley SoftFloat Release 3e's three-clause BSD notice accompanies the runtime. The generated production library records its input digest and the glslang revision selected by the shaderc dependency.
