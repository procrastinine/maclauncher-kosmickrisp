# Mesh pipeline construction

`kk_mesh_build_pipeline` constructs a Metal mesh render pipeline from compiled
task and mesh stages, gathered graphics state and a fragment program. The
monolithic graphics path and the common graphics executable linker use this
entry point. Mesh pipeline construction remains in the mesh capability module;
fragment stage compilation, native fragment linking and executable lifetime
remain with the common stage owner.

```c
VkResult kk_mesh_build_pipeline(struct kk_device *device,
   struct kk_shader *executable,
   const struct msl_compile_data *task,
   const struct msl_compile_data *mesh,
   const struct kk_mesh_fragment_program *fragment,
   bool force_virtual, struct kk_pipeline_handles *pipeline);
```

The declaration is in `vulkan/kk_mesh_pipeline_api.h`, projected from
`src/kosmickrisp/bridge/mtl_bridge.h`.
`executable` contains gathered graphics state and mesh metadata. `task` is null
for a mesh-only executable and otherwise matches its task metadata. `mesh`
contains the shared mesh shader body and entry point. `force_virtual` selects
the compute-task and mesh-only form for a task pipeline. The default selection
uses the task emission bound.

`kk_mesh_fragment_program` accepts two forms. The monolithic form supplies
`monolithic`, a fully linked fragment MSL program with its output operations,
and leaves `attach` null. The native form leaves `monolithic` null and supplies
an `attach` callback with opaque `core` and `epilog` inputs owned by the common
fragment linker. `attach` installs the fragment function and fragment dynamic
linking metadata without changing the object or mesh functions. It retains
native dependencies on the render descriptor so descriptor variants can reuse
them.

After native pipeline creation, optional `finish` constructs fragment function
tables and residency records. `bind_arguments` receives the selected render
pipeline, executable, pipeline handles and argument table for each mesh draw;
the selected render pipeline can be an attachment-format variant. It binds the
fragment tables corresponding to that exact render pipeline. Auxiliary pipeline
kinds distinguish virtual-task pipelines, sliced native pipelines and sliced
virtual-task pipelines: `KK_PIPELINE_MESH_VIRTUAL`, `KK_PIPELINE_MESH_SLICED`
and `KK_PIPELINE_MESH_VIRTUAL_SLICED`. `KK_PIPELINE_KIND_COUNT` bounds descriptor
and fragment-table caches.

The shared `kk_argument_table.h` declaration owns every driver argument entry.
Fragment linking uses `KK_ARGUMENT_FS_VFT_WRAPPER`; mesh parameters use
`KK_ARGUMENT_MESH_PARAMETERS`. Table capacity comes from
`kk_argument_table_buffer_count` with the mesh, fragment-linking and interlock
usage flags. Mesh and fragment code bind their own named entries on that table.

Inputs are borrowed. The builder copies a native fragment recipe for lazy mesh
variants, so its opaque core and epilog objects remain valid until executable
destruction. The common executable owner retains those objects. The caller
zero-initializes `pipeline` and owns every native handle written into it,
including partial results on failure. Common executable cleanup releases main
fragment tables and residency. `release_auxiliary` releases fragment tables
and residency for mesh-owned auxiliary variants, including discarded cache
candidates; mesh code releases their render and compute handles. The builder
does not recompile a native fragment core or copy fragment compiler logic.
