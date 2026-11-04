# Command depth and stencil state

The command interface in `kk_depth_stencil_state.h` exposes the state selected by ordinary graphics binding. `kk_cmd_selected_depth_stencil(cmd, actual_producer)` returns the dynamic state or the attachment-keyed state from the actual vertex or mesh producer. It does not create an encoder or allocate a snapshot.

After the graphics state is flushed, `kk_cmd_capture_depth_stencil` creates an immutable snapshot with:

- The selected native depth/stencil handle and the complete bound `vk_depth_stencil_state`, including front/back stencil operations, masks and references, and depth-bound enable/min/max values.
- The actual producer, command and render encoder; attachment views, formats, layouts, load/store/resolve settings, images, native depth/stencil textures, render area, layer/view masks and sample counts.
- Separate masks for physically present attachments, aspects used by the producer, read-only layouts and memoryless backing, plus memoryless color attachments.

The selected state, encoder and depth/stencil textures are retained with the command. Vulkan objects retain their ordinary application lifetimes. Snapshot storage and lazily compiled variants are released when the command resets or is destroyed, after pending execution and recorded native storage have retired. Retention does not extend the contents of a memoryless attachment beyond its encoder.

`kk_cmd_depth_stencil_variant` supplies `ORIGINAL`, `READ_ONLY` and `BYPASS` states. `ORIGINAL` is the exact selected native handle. `READ_ONLY` preserves comparisons while disabling depth writes, stencil write masks and all stencil update operations. `BYPASS` disables depth/stencil tests and writes. Both optional states use the normal depth/stencil compiler and are created only on request.

`kk_cmd_bind_depth_stencil` binds a mode inside the same live encoder, applies the captured stencil references, and restores the captured depth bounds when supported. `BYPASS` disables depth bounds. Restoring `ORIGINAL` restores the original state, references and bounds. These calls bind state; they perform no tests, draws, barriers, attachment loads/stores or encoder transitions.

An early-test sequence commits original tests and updates in coverage, then runs body and replay with bypassed state using the captured passing samples. A late-test sequence captures geometric coverage with bypassed state, executes application side effects, and applies original tests and updates once in replay after fine fragment outputs are available. Binding original state in both coverage and replay would repeat stencil updates. A read-only comparison is not a substitute for captured passing coverage after an early write. The consumer owns phase scheduling, output/discard/sample-mask handling and restoration; the state interface supplies no cross-draw ordering guarantee.

Ordinary draws create no snapshots or phase variants. The interface changes CPU command/attachment metadata only, with no root-table, descriptor, shader or per-draw upload ABI change.
