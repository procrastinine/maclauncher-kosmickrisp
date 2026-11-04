# Shader feature state and caches

KosmicKrisp shader cache keys distinguish the enabled device features that affect compilation. Pipeline statistics and sampler minimum/maximum filtering have separate bit positions alongside custom borders, null descriptors and image-view minimum LOD.

The shared `kk_shader.h` enumeration assigns bit positions without explicit numeric values. A 32-bit feature mask holds those positions, with a static assertion that the enumeration fits its width. Capability modules consume the shared positions through `BITFIELD_BIT`.

Both graphics and compute shader keys include the enabled-feature mask and the enumeration's count. The count separates cache entries created with the shared layout from entries created with an older layout, and changes automatically when the enumeration grows. Pipeline statistics and sampler reduction remain independent when either, both or neither feature is enabled. This uses the ordinary shader cache and compilation path.
