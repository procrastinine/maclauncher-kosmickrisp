/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_H
#define KK_FRAGMENT_SHADING_RATE_H
#include "nir.h"
#include "kosmickrisp/bridge/mtl_types.h"
struct kk_shader;
mtl_render_pipeline_descriptor *kk_vrs_native_pipeline_descriptor(const struct kk_shader *shader);
#include "kosmickrisp/libkk/kk_fragment_shading_rate_types.h"
/* Accepts explicit-I/O, descriptor-lowered fragment NIR. This bounded internal
 * form supports a single float4 color at 2x2/1sample with no DS/discard exports.
 * Unsupported input is an error to the caller, never a fine-rate substitute.
 * No Vulkan feature or command is exposed by this compiler operation. */
bool kk_nir_lower_coarse_fragment(nir_shader *nir);
bool kk_nir_lower_native_coarse_fragment(nir_shader *nir);
bool kk_nir_vrs_split_application(nir_shader *nir);
void kk_nir_vrs_flatten(nir_shader *nir);
bool kk_nir_lower_linked_native_coarse_fragment(nir_shader *nir);
uint32_t kk_nir_vrs_result_stride(nir_shader *nir);
bool kk_nir_vrs_uses_per_draw(nir_shader *nir);
void kk_nir_vrs_lower_fine_rate(nir_shader *nir);
bool kk_nir_lower_linked_replay_fragment(nir_shader *nir, unsigned samples);
#endif
