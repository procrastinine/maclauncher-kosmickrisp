/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_COMMAND_H
#define KK_FRAGMENT_SHADING_RATE_COMMAND_H
#include "kk_depth_stencil_state.h"
#include "kosmickrisp/bridge/mtl_fragment_shading_rate.h"

/* Internal 32x32 leaf with1/2/4raster samples. Call after graphics-state flush, with
 * the actual selected VS or Mesh producer and an already normalized raster
 * producer. This does not rerun/normalize application pre-raster stages.
 * Tables, context storage and pipelines must be command-resident until GPU
 * completion. All three phases use the same live encoder and attachments. */
VkResult kk_vrs_cmd_encode_native(struct kk_cmd_buffer *cmd,
   struct kk_shader *actual_producer, const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *context);
struct kk_mesh_retained_batch;
/* Synchronous consumer of one GPU-owned original-primitive job. The caller
 * fills shader ID/rate from actual typed records, separately from job identity,
 * and keeps its context/table storage ordered through all three phases.
 * The borrowed batch may not escape its Mesh consume callback. */
VkResult kk_vrs_cmd_encode_mesh_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t job,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_context *context);
struct kk_vrs_mesh_jobs;
struct kk_vrs_prepared;
/* Advance the GPU cursor once, clear its reusable leaf, and consume that
 * original primitive through all phases before any further cursor advance.
 * Reader parameters are immutable and shared across jobs/phases. Scheduling
 * the complete bounded batch/continuation remains the caller's responsibility;
 * no CPU count/completion read is performed by this entry. */
VkResult kk_vrs_cmd_encode_next_mesh_job(struct kk_cmd_buffer *,
   const struct kk_mesh_retained_batch *, const struct kk_vrs_mesh_jobs *,
   const struct mtl_vrs_pipelines *, mtl_argument_table *original_fragment,
   mtl_argument_table *coarse_fragment, const struct kk_vrs_prepared *);
struct kk_poly_retained_raster;
/* Prepare the immutable GPU range and open/flush graphics before entering this
 * leaf. Pipelines use the actual retained Poly FS convention and descriptor.
 * All original subdivisions remain in the same indirect primitive job. */
VkResult kk_vrs_cmd_encode_poly_job(struct kk_cmd_buffer *cmd,
   const struct kk_poly_retained_raster *raster, uint64_t indirect_draw,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_fragment, mtl_argument_table *coarse_fragment,
   const struct kk_vrs_context *context);
#endif
