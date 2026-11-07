# Retained raster outputs

Poly captures ordinary vertex, tessellation and geometry outputs in original primitive order. Raster consumers reuse the capture's linked varying values, clipping results and inclusive packet prefix. The native reader contains no application producer body or transform-feedback writes. Point output uses the native point reader; line and polygon expansion retain their original primitive's complete output range.

A retained consumer selects one assembled primitive or an explicit contiguous batch through a GPU-resident range and indirect draw. Empty and culled primitive slots produce empty draws. Range indices identify ordered capture slots; application PrimitiveID remains the original linked value. Original draw parameters, Layer, ViewIndex and primitive shading-rate outputs remain in the existing per-draw and captured records.

All consumers finish before another original draw or slice overwrites the retained output. Multi-phase raster consumers prepare their range jobs before entering the live render encoder and reuse the same job for every phase. Their fragment functions and stage-private arguments remain separate from the original producer parameters. The nullable logical-Layer selector remains two words at argument 7, and the interlock guard remains at argument 6.
