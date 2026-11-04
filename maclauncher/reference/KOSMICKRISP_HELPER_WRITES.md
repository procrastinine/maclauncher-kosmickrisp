# Fragment helper memory effects

MacLauncher KosmicKrisp suppresses external memory effects from fragment helper
invocations after discard or demotion. Fragment shaders that can demote or
terminate and write memory use Mesa's helper-write lowering after API-specific
shader transformations, including generated polygon and culling operations.

The lowering guards ordinary storage-buffer stores, storage-image writes and
atomics. Helper state includes the invocation's initial state and subsequent
demotion. A skipped atomic result is zero. Termination keeps its control-flow
exit, so later instructions do not execute in that invocation.

The same path serves pixel and sample shading. Demoting shaders that query their
helper status track it even when they write only colour outputs. Shaders without
these control operations, and demoting shaders without memory writes or helper
queries, use their existing Metal source generation.

The adaptation is the `helper-side-effects` correction in the
[source catalog](../../src/kosmickrisp/).
