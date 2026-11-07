# Shader caches

The device cache reuses live shaders and their native pipeline objects. The physical-device disk cache uses the driver build identity and GPU settings to separate compiled shader records. Mesa controls cache storage and its default size limit.

Embedded sampler metadata is read through the shader stage's existing root table. Native shader records contain a versioned binding schema and canonical sampler states, including their existing border and conversion state. CPU pointers and GPU metadata addresses are excluded. Deserialization rebuilds the current device's immutable metadata table before returning the shader. The shader owns its canonical CPU state; the existing device arena retains its GPU metadata.

A compiler record identifies embedded sampler use. Serialization rejects such a shader if its runtime binding state is incomplete. Legacy schemas, changed state layouts, truncated binding records and damaged canonical-state bytes are rejected before native code compilation. The in-process cache retains its existing reference and lifetime rules.
