# Indirect tessellation

MacLauncher KosmicKrisp preserves vertex, patch and instance ownership for `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect` and their indirect-count forms when tessellation is active.

The software vertex pass dispatches exactly the input vertex count for each instance. The tessellation-control pass uses one workgroup per input patch, with the output patch size as its local width. Partial vertex workgroups retain the same invocation behavior as direct draws.

GPU setup writes three indirect thread-dispatch records for the vertex, tessellation-control and tessellator passes. Each record contains three global dimensions followed by three local dimensions. The host and compute setup share the 24-byte record definition and use Metal's indirect thread-dispatch operation.

The implementation is listed in the [source adaptation catalog](../../src/kosmickrisp/).
