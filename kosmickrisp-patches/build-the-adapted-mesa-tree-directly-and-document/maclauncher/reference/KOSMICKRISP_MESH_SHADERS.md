# Task and mesh shaders

MacLauncher KosmicKrisp implements `VK_EXT_mesh_shader` with Metal object and
mesh functions. Mesh output declarations supply the native topology and the
maximum vertex and primitive counts. Per-vertex attributes and per-primitive
attributes use the same varying names as fragment inputs. Task payload memory
connects the task and mesh stages.

Fragment interfaces that exceed the native varying capacity use the canonical
heap transport. After the mesh body completes, the producer writes a three-corner
packet with one 16-byte cell per generic location. Primitive indices select the
staged vertex values; per-primitive values repeat at each corner. The shared
fragment loader reads this packet without running the mesh body again.

Interpolated interfaces append the original homogeneous corner positions to the
same packet. An ordinary native position carrier evaluates the requested center,
centroid, sample or offset location. The shared loader reconstructs perspective
or no-perspective weights from that location and the original corners. Shared
mesh vertices retain their original indices and vertex count.

General interpolated overflow uses bounded physical batches. The batch span
comes from the shared heap capacity, the declared primitive count and the packet
size. Completed native rendering precedes each heap reset; native render
restarts retain attachment contents. Logical workgroup IDs, grid sizes and task parentage remain
unchanged across batches. Interfaces within the native capacity retain their
native varying declarations and scheduling, including the two GPU-generated
slices for mesh-only indirect draws.

Mesh-only pipelines use native direct or indirect mesh dispatch. A task shader
also uses the native object stage when NIR range analysis proves that every
`EmitMeshTasksEXT` emits at most 1024 mesh workgroups in total. The proof checks
all three dimensions without allowing multiplication overflow.

Other task shaders run as compute in batches of at most 256 task workgroups.
Each workgroup writes its payload and emitted grid. An ordered GPU prefix scan
builds one mesh-only indirect grid per batch. Each mesh workgroup finds its
parent task through the prefix array and reconstructs its local mesh workgroup
ID. Batches execute in task order and reuse a fixed payload arena. A draw with
4,194,304 task workgroups therefore uses the same payload arena as a small draw.
Indirect draw arguments, indirect counts and conditional rendering predicates
are read on the GPU.

Task and mesh invocation statistics count logical workgroups. Mesh primitive
queries count the primitives declared by `SetMeshOutputsEXT`. Padding in the
physical indirect grid contributes no invocations or primitives.

The task payload limit is 16,384 bytes. Task and mesh grids support up to
4,194,304 workgroups in total, with a limit of 65,535 in each dimension. Task
and mesh local sizes support 128 invocations, with a limit of 128 in each
dimension. Mesh declarations support 256 vertices and 512 primitives.

The compiler, command state, descriptors, attachment configuration and shader
cache are shared with ordinary graphics pipelines. Mesh scheduling resources
are allocated when mesh command encoding needs them.
