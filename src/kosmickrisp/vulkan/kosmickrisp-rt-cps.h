/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_CPS_H
#define KK_RT_CPS_H
#include "nir/nir.h"
#include <stdint.h>
#include <stddef.h>

/* One invocation's persistent control block. The visible-function caller passes
 * its address as the sole64-bit NIR parameter; no Metal buffer slot is reserved.
 * Before invoking an entry/resume, the scheduler sets action to RETURN and
 * restores that invocation's frame_base/payload/hit state. */
#include "kosmickrisp/vulkan/kk_rt_abi.h"

struct kk_rt_shader_parts {
   nir_shader *entry;
   nir_shader **resumes;
   uint32_t resume_count;
   uint32_t scratch_size;
   uint64_t payload_size;
   uint64_t hit_attribute_size;
};
/* Query state must already have passed the shared RQ lowering.
 * Consumes shader on success, retaining ownership in parts. Allocation belongs
 * to the common NIR memory context; the caller compiles each part separately. */
bool kk_rt_split_shader(nir_shader *shader, void *mem_ctx,
                        struct kk_rt_shader_parts *parts);
bool kk_rt_prepare_context(nir_shader *shader);
struct nir_builder;
nir_def *kk_rt_load_embedded_sampler_table(struct nir_builder *b);
bool kk_rt_shader_frame_fits(nir_shader *shader);
/* Internal bit of pipeline_ray_flags, outside the SPIR-V ray flags: the
 * pipeline was created with the opacity micromap flag. */
#define KK_RT_PIPELINE_OPACITY_MICROMAP (1u << 31)
nir_shader *kk_rt_build_traversal(void *mem_ctx, uint32_t pipeline_ray_flags);
bool kk_rt_split_shader_mode(nir_shader *shader, void *mem_ctx, struct kk_rt_shader_parts *parts, bool internal);
#endif
