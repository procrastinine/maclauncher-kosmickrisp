/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_PIPELINE_API_H
#define KK_MESH_PIPELINE_API_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#include "kosmickrisp/bridge/mtl_types.h"
#include "kosmickrisp/libkk/kk_mesh_types.h"

struct kk_device;
struct kk_shader;
struct kk_pipeline_handles;
struct msl_compile_data;

/* Mesh parameters occupy slot 5. Slots 3 and 4 remain available to the common
 * fragment linker. Ordinary graphics argument tables are unaffected. */
struct kk_mesh_fragment_program {
   /* Exactly one of monolithic and attach is set. Monolithic MSL already
    * includes its fragment output operations. The native form carries the
    * common linker's compiled core and state-dependent epilog. */
   const struct msl_compile_data *monolithic;
   /* The native core/epilog owner supplies the actual application-use flag.
    * Monolithic programs use their compiled metadata instead. */
   bool uses_pipeline_statistics;
   /* attach leaves sampler10 unspecialized; the common builder supplies the
    * effective value while preserving all other constants. */
   bool uses_sampler_emulation;
   bool requires_sampler_emulation;
   const void *core;
   const void *epilog;
   VkResult (*attach)(struct kk_device *device,
      const struct kk_mesh_fragment_program *fragment,
      mtl_render_pipeline_descriptor *descriptor);
   VkResult (*finish)(struct kk_device *device,
      const struct kk_mesh_fragment_program *fragment,
      mtl_render_pipeline_descriptor *descriptor,
      struct kk_pipeline_handles *pipeline, unsigned kind);
   void (*bind_arguments)(const struct kk_mesh_fragment_program *fragment,
      const struct kk_shader *executable,
      struct kk_pipeline_handles *pipeline, unsigned kind,
      mtl_render_pipeline_state *render,
      mtl_argument_table *table);
   /* Releases only the fragment tables/residency of a mesh-owned auxiliary
    * variant. The common executable destructor releases the main variant. */
   void (*release_auxiliary)(struct kk_device *device,
      const struct kk_mesh_fragment_program *fragment,
      struct kk_pipeline_handles *pipeline, unsigned kind);
};

struct kk_cmd_buffer;
/* Begin after resolving the original Mesh executable and binding layer0.
 * The callback owns only attachments. End at every original draw exit. */
bool kk_mesh_layer_replay_begin(struct kk_cmd_buffer *cmd, uint32_t layers,
   VkResult (*bind)(struct kk_cmd_buffer *, void *, uint32_t), void *data);
void kk_mesh_layer_replay_end(struct kk_cmd_buffer *cmd);
/* During an active replay the normal Mesh entry points retain one bounded
 * slice, then invoke bind for all layers before reusing any cache/Poly heap
 * contents. The cache belongs to the recording pool through retirement;
 * variants and their fragment dependencies belong to the executable. */
VkResult kk_mesh_get_retained_pipeline(struct kk_device *, struct kk_shader *,
   bool reader, struct kk_pipeline_handles **);

/* A bounded batch contains all original primitive slots of one physical
 * slice, for one logical draw/task batch/view. Counts remain in GPU memory.
 * Identity is (per_draw_address, id, view_index, logical_layer). Reused
 * output addresses alone are NOT batch identities. The GPU grid determines
 * valid groups; unused cache headers may retain a previous slice. */
struct kk_mesh_retained_batch_id {
   uint32_t draw_id, task_base, slice, group_base;
};
struct kk_mesh_retained_layout {
   uint32_t group_stride;
   uint32_t vertices, vertex_stride;
   uint32_t primitives, primitive_stride;
   uint32_t indices, layers, vertices_per_primitive;
   uint32_t max_vertices, max_primitives;
   struct kk_mesh_retained_field fields[KK_MESH_RETAINED_FIELD_COUNT];
};
struct kk_mesh_retained_batch {
   const struct kk_shader *producer;
   mtl_render_pipeline_descriptor *raster_descriptor; /* borrowed, read-only */
   mtl_render_pipeline_state *raster_pipeline;
   mtl_render_encoder *encoder;
   mtl_argument_table *mesh_table; /* no consumer mutation */
   uint64_t outputs, output_bytes, grid, reader_parameters;
   uint64_t layer_selector, per_draw_address, task_records;
   struct kk_mesh_retained_batch_id id;
   struct kk_mesh_retained_layout layout;
   uint32_t group_capacity, logical_layer, view_index;
   bool layer_filtered;
};
typedef VkResult (*kk_mesh_retained_consume)(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, void *);
struct kk_mesh_retained_consumer {
   kk_mesh_retained_consume consume;
   void *data;
   bool transaction, saved_heap_active, saved_heap_used;
   bool callback_active;
};
/* Register after the original executable flush, around exactly one original
 * draw. The synchronous callback encodes every phase before returning. It
 * may not close/change the live encoder, rerun a producer, or reset arenas.
 * Batch/table/selector records are borrowed and command-retired; the CPU batch
 * descriptor itself lives only through the callback. */
bool kk_mesh_retained_raster_begin(struct kk_cmd_buffer *,
   kk_mesh_retained_consume, void *);
void kk_mesh_retained_raster_end(struct kk_cmd_buffer *);
mtl_render_pipeline_descriptor *kk_mesh_retained_raster_descriptor(
   const struct kk_shader *);
/* The supplied phase pipeline keeps the exact retained reader and linked IO.
 * Only the FRAGMENT table is replaced; Mesh slot5 retains its existing cache
 * address/logical-Layer meaning. The unchanged GPU grid/counts are used for
 * every phase. All original slots/indices/IDs and primitive-rate fields remain
 * in their typed output records, including culled slots. */
VkResult kk_mesh_retained_raster_replay(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, mtl_render_pipeline_state *,
   mtl_argument_table *fragment_table);

/* Narrow primitive-job control. VRS owns GPU job enumeration and job storage.
 * job points to an immutable/ordered kk_mesh_retained_job prefix associated
 * with this exact batch identity, not a shader PrimitiveID. The returned
 * slot5 parameters are uploaded once and command-retired, reusable by all
 * phases. No CPU reads of the job, original grid or count header occur. */
VkResult kk_mesh_retained_raster_job_parameters(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, uint64_t job, uint64_t *parameters);
VkResult kk_mesh_retained_raster_replay_job(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, uint64_t parameters,
   mtl_render_pipeline_state *, mtl_argument_table *fragment_table);

/* Construct a capture/transaction descriptor from the actual retained Mesh
 * reader, never an application Mesh stage or Poly vertex wrapper. fragment
 * is an already compiled caller/common-cache function. The returned +1 native
 * descriptor retains its reader/library and fragment dependencies; caller
 * releases it with mtl_release and owns attachment/DS/capture policy. No
 * application stages, native library compilation or cache is created here.
 * Existing replay_job encodes this descriptor's pipeline with original
 * primitive-slot identity and inputs; DS snapshot/transaction/replay remain
 * the transaction consumer's responsibility. */
mtl_render_pipeline_descriptor *kk_mesh_retained_fragment_descriptor(
   const struct kk_mesh_retained_batch *, mtl_function_descriptor *fragment);

/* Synchronous transaction recording mode: this callback may close/restart
 * encoders using the existing preserved-store/private-backing path. Before
 * each source capture, resume supplies the exact current encoder while all
 * original source records/parameters remain unchanged. Encode all Mesh reads
 * inside this callback, BEFORE its corresponding existing transaction split;
 * only independently owned capture/DS packets may survive callback return.
 * Existing continuations order later source reads before chunk/arena reuse.
 * There is no escaping Mesh snapshot or asynchronous Mesh reader contract. */
bool kk_mesh_retained_transaction_begin(struct kk_cmd_buffer *,
   kk_mesh_retained_consume, void *);
void kk_mesh_retained_transaction_end(struct kk_cmd_buffer *);
VkResult kk_mesh_retained_transaction_resume(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, struct kk_mesh_retained_batch *current);

/* executable supplies gathered graphics state and mesh metadata. task is NULL
 * for mesh-only pipelines. Inputs are borrowed; a native fragment recipe is
 * copied for lazy variants, so its core/epilog objects must remain valid until
 * executable destruction. The common stage owner retains those objects.
 *
 * executable->info.uses_per_draw_data includes all linked Task/Mesh/FS inputs,
 * computed once when the executable is gathered.
 *
 * executable->mesh borrows Task/Mesh state for this call; only its numeric
 * info member is serialized. Its runtime pointer owns lazy native handles.
 * Compiler-created shaders keep it in their original allocation; temporary
 * variants use a local value copy so their flags cannot alter the producer.
 *
 * pipeline is zero-initialized by the caller. Every handle written into it,
 * including partial results on failure, belongs to the caller. attach retains
 * its native dependencies on the descriptor. finish owns fragment tables and
 * residency through the common executable cleanup path. */
VkResult kk_mesh_build_pipeline(struct kk_device *device,
   struct kk_shader *executable,
   const struct msl_compile_data *task,
   const struct msl_compile_data *mesh,
   const struct kk_mesh_fragment_program *fragment,
   bool force_virtual, struct kk_pipeline_handles *pipeline);

#endif
