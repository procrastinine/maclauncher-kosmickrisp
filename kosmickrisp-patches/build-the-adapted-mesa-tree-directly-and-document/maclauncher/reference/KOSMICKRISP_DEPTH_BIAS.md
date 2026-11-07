# KosmicKrisp depth bias

The MacLauncher KosmicKrisp adaptation provides `VK_EXT_depth_bias_control` for the depth-bias representations used by Direct3D translators. Static pipeline state and `vkCmdSetDepthBias2EXT` use the same implementation.

| Representation | Constant-bias unit |
| --- | --- |
| `LEAST_REPRESENTABLE_VALUE_FORMAT_EXT` | Format spacing; floating-point depth uses the maximum exponent of the clipped polygon. |
| `LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT` | `2^-n`, with the depth bit count for fixed-point formats and mantissa bits plus one for floating-point formats. |
| `FLOAT_EXT` | One unit of framebuffer depth. |

`depthBiasExact` selects the exact unit. The constant and slope terms are added before the signed bias clamp is applied. A representation applies to the actual Vulkan depth attachment format; this extension does not add D24 attachment formats.

Ordinary depth bias retains Metal's native state. For explicit representations and exact bias on triangles, Mesa Poly captures the original geometry, clips it, computes one bias for each clipped polygon, and emits triangles, boundary lines or point squares according to polygon mode. The bias is applied after clipping, and native depth bias is disabled for this path. Original flat values, provoking vertices, geometry streams, transform feedback and polygon facing follow the shared Poly implementation.

Depth bias control extends the polygon implementation in `src/kosmickrisp/vulkan/`.
