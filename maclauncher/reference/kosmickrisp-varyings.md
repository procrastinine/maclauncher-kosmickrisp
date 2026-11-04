# Vertex-fragment interfaces

MacLauncher KosmicKrisp preserves the exposed 128-component vertex-output and fragment-input limits. Interfaces exceeding 124 native varying components, including raster system inputs, use the shared primitive-corner transport to make space for position and interpolation inputs.

Selected ordinary inputs are stored once per primitive corner in the GPU heap. Fragment shaders reconstruct their original flat, perspective, no-perspective, centroid, sample or explicit-offset interpolation from native basis interpolants. Geometry and tessellation retain their earlier Poly capture and transform-feedback data; only the final native rasterization interface is packed.

Smaller interfaces retain their ordinary vertex and fragment shaders, draw topology and per-draw uploads. The [full-interface fix](../../src/kosmickrisp/) and [barycentric capability](../../src/kosmickrisp/) share one transport implementation.
