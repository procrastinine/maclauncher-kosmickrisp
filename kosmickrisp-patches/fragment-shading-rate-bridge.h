/* SPDX-License-Identifier: MIT */
#ifndef MTL_FRAGMENT_SHADING_RATE_H
#define MTL_FRAGMENT_SHADING_RATE_H
#include "mtl_types.h"
#include <stdbool.h>
#include "kosmickrisp/libkk/kk_fragment_shading_rate_types.h"
struct mtl_vrs_pipelines;
/* Called only for an explicitly requested coarse execution artifact. Ordinary
 * pipelines do not invoke this or retain any Metal variant through it. */
struct mtl_vrs_pipelines *mtl_vrs_new_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline, const char *coarse_msl,
   const char *coarse_entrypoint);
void mtl_vrs_destroy_pipelines(struct mtl_vrs_pipelines *pipelines);
/* Internal bounded form: 32x32, one API triangle, 2x2, one sample, RGBA32F.
 * Tables and resources are supplied/resident through the command owner; the
 * same original native vertex descriptor is used by coverage and replay. */
bool mtl_vrs_encode_2x2(mtl_render_encoder *encoder,
   const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *context);

struct mtl_vrs_pipelines *mtl_vrs_new_native_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline, const char *msl, const char *entry,
   const char *replay_msl, const char *replay_entry, bool replay_framebuffer_reads,
   enum mtl_language_version version, enum mtl_math_mode mode,
   enum mtl_math_floating_point_functions functions);
/* Reuses the exact library-function descriptors returned by the common cache;
 * it does not compile application/replay libraries a second time. */
struct mtl_vrs_pipelines *mtl_vrs_new_cached_native_pipelines(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *native_pipeline,
   mtl_function_descriptor *body, mtl_function_descriptor *replay,
   bool framebuffer_reads, enum mtl_language_version version,
   enum mtl_math_mode math, enum mtl_math_floating_point_functions functions,
   uint32_t result_stride, bool uses_per_draw, bool early_fragment_tests);
enum mtl_vrs_phase { MTL_VRS_COVERAGE, MTL_VRS_BODY, MTL_VRS_REPLAY };
bool mtl_vrs_pipelines_early_tests(const struct mtl_vrs_pipelines *pipelines);
uint32_t mtl_vrs_pipelines_samples(const struct mtl_vrs_pipelines *pipelines);
/* Borrowed pipeline for a reader-owned raster draw; the bundle remains live.
 * Ordering is the existing same-tile dependency, not a global batch fence. */
mtl_render_pipeline_state *mtl_vrs_phase_pipeline(const struct mtl_vrs_pipelines *,
   enum mtl_vrs_phase phase);
bool mtl_vrs_order_native_phase(mtl_render_encoder *, const struct mtl_vrs_pipelines *,
   enum mtl_vrs_phase phase);
/* The command owner binds the selected DS mode before each phase. */
bool mtl_vrs_encode_native_phase(mtl_render_encoder *encoder,
   const struct mtl_vrs_pipelines *pipelines, mtl_argument_table *original_table,
   mtl_argument_table *coarse_table, const struct kk_vrs_context *context,
   enum mtl_vrs_phase phase);
/* GPU-only metadata copy and scratch clear in the current encoder. */
bool mtl_vrs_prepare_mesh_job(mtl_render_encoder *, const struct mtl_vrs_pipelines *,
   mtl_argument_table *private_table, uint64_t parameters, uint64_t context,
   uint32_t pixels);
bool mtl_vrs_encode_native(mtl_render_encoder *encoder, const struct mtl_vrs_pipelines *pipelines,
   mtl_argument_table *original_table, mtl_argument_table *coarse_table,
   const struct kk_vrs_context *context);
#endif
