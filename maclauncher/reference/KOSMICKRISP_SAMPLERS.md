# KosmicKrisp samplers

Custom border colours are reconstructed from Metal samplers with zero and one borders. Ordinary colour sampling combines the two filtered results; integer sampling and gather select border components without interpolation. Fetch operations retain their ordinary texture-load path.

Components with identical zero-border and one-border results preserve their original bits. Float opaque-black depth comparison uses Metal's native zero depth border, while colour sampling retains the RGBA4 opaque-black correction.

Arbitrary custom border values are unsupported for depth comparison.

Sampler objects share Metal states through Mesa's sampler heap. A custom colour uses two heap entries when its zero and one states are distinct; predefined depth comparison needs one. Custom-border support does not change the hardware sampler capacity or the reported device limits.

Per-sampler [cube face filtering](KOSMICKRISP_CUBE_FILTERING.md) provides seamless and non-seamless cube sampling through ordinary Metal texture views.
