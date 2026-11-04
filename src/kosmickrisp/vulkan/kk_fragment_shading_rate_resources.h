/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_RESOURCES_H
#define KK_FRAGMENT_SHADING_RATE_RESOURCES_H
#include "kk_cmd_buffer.h"
#include "kosmickrisp/libkk/kk_fragment_shading_rate_types.h"

struct kk_shader;
struct kk_vrs_prepared {
   const struct kk_vrs_context *context;
   uint64_t context_gpu;
   uint32_t bytes;
};

/* Allocate a single-use bounded leaf from the existing command uploader.
 * Original/root/sampler/linked bindings already belong to the caller's stage
 * tables. Only the separate coarse table's PER_DRAW entry changes here.
 * original_per_draw is the actual ordinary upload address, never a copy of its
 * contents. The returned resources retain the normal command-pool lifetime.
 * This is not the arbitrary-window/indirect continuation scheduler. */
VkResult kk_vrs_cmd_prepare_native(struct kk_cmd_buffer *cmd,
   struct kk_shader *fragment, const struct kk_vrs_context *parameters,
   uint64_t original_per_draw, mtl_argument_table *original_table,
   mtl_argument_table *coarse_table, struct kk_vrs_prepared *out);
struct kk_mesh_retained_batch;
struct mtl_vrs_pipelines;
/* Read actual typed fields/guards on the GPU and clear a reusable leaf before
 * its next job. Remains in the live encoder; no job/count CPU dereference. */
VkResult kk_vrs_cmd_prepare_mesh_job(struct kk_cmd_buffer *cmd,
   const struct kk_mesh_retained_batch *batch, uint64_t job,
   const struct mtl_vrs_pipelines *pipelines, mtl_argument_table *private_table,
   const struct kk_vrs_prepared *resources);
#endif
