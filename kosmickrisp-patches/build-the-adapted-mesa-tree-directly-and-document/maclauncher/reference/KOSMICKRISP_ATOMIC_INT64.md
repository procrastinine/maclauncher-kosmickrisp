# 64-bit integer atomics

MacLauncher KosmicKrisp supplies `VK_KHR_shader_atomic_int64` with buffer and workgroup-memory atomics. Supported integer operations include addition, signed and unsigned minimum and maximum, bitwise operations, exchange, compare-exchange, atomic loads and atomic stores. Full-width return values and carry between the two 32-bit halves are preserved.

A device-owned hashed lock table serializes each target address. The same retry loop reads or updates both halves and releases its lock before participating lanes reconverge. Retry-loop results use a pair of 32-bit values and reconstruct the 64-bit value after leaving the loop. Nonreturning unsigned buffer minimum and maximum use Metal's native `atomic_ulong` operations under the same lock. Fragment helper invocations skip lock acquisition.

The lock table allocates 256 KiB when the device enables either atomic feature. Its address uses the existing descriptor root word; root upload sizes are unchanged. Shaders without 64-bit atomics omit the helper. The adaptation is the `atomic-int64` capability in the [source catalog](../../src/kosmickrisp/).
