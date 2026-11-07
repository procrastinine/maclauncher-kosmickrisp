# Line rasterization

Wide lines use Mesa Poly's shared primitive capture, clipping, interpolation and
rasterization path. Rectangular lines are expanded to quads in framebuffer
coordinates. Bresenham lines use parallel native line replicas, preserving the
native diamond coverage rule. Static pipeline widths and dynamic line widths
use the same expansion state. Polygon outlines share the same path.

| Property | Value |
| --- | --- |
| `wideLines` | Supported |
| `rectangularLines` | Supported |
| `smoothLines` | Supported |
| `bresenhamLines` | Supported |
| `stippledRectangularLines` | Unsupported |
| `stippledBresenhamLines` | Supported |
| `stippledSmoothLines` | Unsupported |
| `strictLines` | False |
| `lineWidthRange` | 1 through 4096 pixels |
| `lineWidthGranularity` | 1/256 pixel |

Requested widths are rounded to the nearest 1/256 pixel within the reported
range. Bresenham coverage rounds the resulting width to an integer replica
count. Ordinary default and Bresenham lines of width 1 use native rendering.
Explicit rectangular lines retain rectangular coverage at width 1. Non-strict
rasterization preserves the native unit-width path without additional GPU
dispatches, primitive storage or synchronization. Dynamic-width pipelines
prepare a native vertex and fragment program pair for this path.

Smooth lines multiply fragment alpha by rectangle coverage before blending.
Other line modes retain their ordinary alpha. Bresenham stipple supports the
16-bit pattern and repeat factors 1 through 256, including dynamic state. Strip
segments continue the counter; primitive restart begins a new counter.
Span preparation assigns a separate workgroup to each original primitive and
uses the shared Poly prefix compaction.

Stipple precedes observable early depth and stencil tests. Eligible fragment
shaders use an initial NIR discard, with ignored depth and stencil-reference
exports removed when forced early tests are converted to late tests.

Shaders with observable early-test interactions retain forced early tests.
Poly emits only ON Bresenham coverage before fragment processing, retaining
the native pixel coverage rule.
