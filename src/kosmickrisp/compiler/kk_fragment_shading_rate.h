/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_H
#define KK_FRAGMENT_SHADING_RATE_H
#include "nir.h"
#include "kosmickrisp/bridge/mtl_types.h"
struct kk_shader;
struct nir_builder;
mtl_render_pipeline_descriptor *kk_vrs_native_pipeline_descriptor(const struct kk_shader *shader);
#include "kosmickrisp/libkk/kk_fragment_shading_rate_types.h"
/* Accepts explicit-I/O, descriptor-lowered fragment NIR. This bounded internal
 * form supports a single float4 color at 2x2/1sample with no DS/discard exports.
 * Unsupported input is an error to the caller, never a fine-rate substitute.
 * No Vulkan feature or command is exposed by this compiler operation. */
/* Uses the real context's pipeline/primitive state and checked R8 texture
 * fetch. absolute_pixel retains the rate image's framebuffer coordinates. */
nir_def *kk_nir_vrs_combined_extent(struct nir_builder *, nir_def *context,
   nir_def *absolute_pixel, unsigned samples);
bool kk_nir_lower_coarse_fragment(nir_shader *nir);
bool kk_nir_lower_native_coarse_fragment(nir_shader *nir);
bool kk_nir_vrs_split_application(nir_shader *nir);
void kk_nir_vrs_flatten(nir_shader *nir);
bool kk_nir_lower_linked_native_coarse_fragment(nir_shader *nir);
uint32_t kk_nir_vrs_result_stride(nir_shader *nir);
/* Invocation-local typed backup for the same-transaction fine epilogue.
 * These compilation-clone functions contain no application call or external
 * memory access. The caller supplies the original-pixel mask shift. */
struct kk_vrs_local_outputs {
   nir_function *save, *restore;
   unsigned bytes;
};
bool kk_nir_vrs_local_outputs(nir_shader *, nir_variable *sample_shift,
   unsigned samples, struct kk_vrs_local_outputs *);
bool kk_nir_vrs_uses_per_draw(nir_shader *nir);
void kk_nir_vrs_lower_fine_rate(nir_shader *nir);
bool kk_nir_lower_linked_replay_fragment(nir_shader *nir, unsigned samples);
/* Borrowed shader globals filled by the shared transaction before its sole
 * call. Fine inputs/exports/DS/demotion stay in that existing wrapper. */
struct kk_vrs_logical_variables {
   nir_variable *logical_lane, *initial_helper, *rate, *original_per_draw;
};
bool kk_nir_vrs_lower_transaction_body(nir_shader *, nir_function *application,
   const struct kk_vrs_logical_variables *);
bool kk_nir_vrs_rebase_fine_draw(nir_function *, nir_variable *original_per_draw);
/* Compilation clone only. Keep the actual named body and typed globals;
 * remove its sole call from the existing fine wrapper without inlining or
 * repeating any finishing pass. The caller later selects its transaction
 * entry and calls application once, fine epilogue per original sample. */
bool kk_nir_vrs_take_fine_epilogue(nir_shader *, nir_function **application,
   nir_function **epilogue);
/* Caller owns liveness/coverage, pointer validity and ordered packet lifetime.
 * Load restores every typed application output before the fine callable and
 * slices SampleMask by its original coarse-pixel/sample bit position. */
bool kk_nir_vrs_store_payload(struct nir_builder *, nir_def *result);
bool kk_nir_vrs_load_payload(struct nir_builder *, nir_def *result,
   nir_def *sample_shift, unsigned samples);
#endif
