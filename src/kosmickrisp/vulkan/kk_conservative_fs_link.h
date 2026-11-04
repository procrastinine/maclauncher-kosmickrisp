/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "nir.h"
struct kk_per_vertex_map;
struct kk_shader;
struct poly_gs_capture_info;
struct msl_fragment_link_info;

enum kk_conservative_input_evaluation {
   KK_CONSERVATIVE_INPUT_FLAT,
   KK_CONSERVATIVE_INPUT_CENTER_SMOOTH,
   KK_CONSERVATIVE_INPUT_CENTER_LINEAR,
   KK_CONSERVATIVE_INPUT_CENTROID_SMOOTH,
   KK_CONSERVATIVE_INPUT_CENTROID_LINEAR,
   KK_CONSERVATIVE_INPUT_SAMPLE_SMOOTH,
   KK_CONSERVATIVE_INPUT_SAMPLE_LINEAR,
   KK_CONSERVATIVE_INPUT_CANONICAL_ADDRESS,
   KK_CONSERVATIVE_INPUT_CANONICAL_SMOOTH_BASIS,
   KK_CONSERVATIVE_INPUT_CANONICAL_LINEAR_BASIS,
};

struct kk_conservative_fs_input {
   const char *member; /* exact member in KKCoreFragmentIn */
   nir_alu_type type;
   uint8_t location;
   uint8_t component;
   uint8_t components;
   enum kk_conservative_input_evaluation evaluation;
};

struct kk_conservative_fs_builtin {
   gl_system_value semantic;
   const char *argument;
};

struct kk_conservative_fs_link_info {
   /* Apply after the existing GPL output epilog has been composed. */
   const char *native_wrapper;
   const char *stage_in_type;
   const char *stage_in_argument;
   const char *sample_mask_output_member; /* NULL when the final wrapper has none */
   const struct kk_conservative_fs_builtin *builtins;
   uint32_t builtin_count;
   bool sample_shading;
   bool early_fragment_tests;
   const struct msl_fragment_link_info *native_info;
   const struct kk_conservative_fs_input *inputs;
   uint32_t input_count;
   const struct kk_per_vertex_map *canonical;
   /* Optional MSL int4 expression: logical pixel bounds (x0,y0,x1,y1).
    * NULL selects the 1x1 fragment only while coarse rates are unadvertised. */
   const char *logical_fragment_rectangle;
};

/* Borrow all inputs for the call. Success returns complete MSL allocated
 * under mem_ctx; it invokes the original native FS binary exactly through
 * its retained visible-function interface. No NIR input or retention. */
VkResult kk_conservative_build_fs_prolog(
   void *mem_ctx, const struct kk_conservative_fs_link_info *fragment,
   const struct poly_gs_capture_info *producer, char **out_wrapper);

/* Explicit-interpolation libraries already retain lowered NIR. The caller
 * clones that existing NIR and passes the clone here; this pass changes it
 * in place and never retains it. The caller compiles and frees the clone. */
bool kk_conservative_lower_canonical_fs(
   nir_shader *fragment, const struct kk_per_vertex_map *canonical,
   const struct poly_gs_capture_info *producer);

/* Set only on the final executable, never on a shared immutable library. */
void kk_conservative_require_canonical(struct kk_shader *pipeline,
                                       uint64_t generic_locations);

/* Gather from IO-lowered FS before canonical lowering. Retain only the two
 * words; the final executable setter copies them without retaining NIR. */
#define KK_CONSERVATIVE_CANONICAL_COMPONENTS_API 1
void kk_conservative_gather_canonical_components(nir_shader *fragment,
                                                 uint64_t components[2]);
void kk_conservative_require_canonical_components(struct kk_shader *pipeline,
   uint64_t generic_locations, const uint64_t components[2]);

#define KK_CONSERVATIVE_INNER_COVERAGE_API 1
struct nir_builder;
nir_def *kk_conservative_evaluate_inner_coverage(struct nir_builder *builder,
   nir_def *packet, nir_def *logical_rectangle);
/* Request optional inner edges on the final executable only. */
void kk_conservative_require_inner_coverage(struct kk_shader *executable);
