/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_WORKSPACE_H
#define KK_FRAGMENT_SHADING_RATE_WORKSPACE_H
#include "kk_depth_bounds_types.h"
#include "kosmickrisp/libkk/kk_fragment_shading_rate_types.h"
#include "kosmickrisp/libkk/kk_poly_retained.h"
#include <string.h>

#define KK_VRS_WORKSPACE_BUDGET (64u * 1024u * 1024u)
#define KK_VRS_WORKSPACE_MAX_JOBS 32u

/* One reusable window, not a framebuffer-by-primitive allocation. Capacity is
 * storage only: the GPU cursor must continue through every original job and
 * window. The scheduler deducts its other bounded storage from byte_budget. */
struct kk_vrs_workspace_layout {
   uint32_t bytes, byte_budget, requested_jobs;
   uint32_t side, rows, jobs, samples, input_stride;
   uint32_t envelope, contexts, job_records, cursor;
   uint32_t inputs, claims, coverage, metadata, depth, stencil, color[8];
};

static inline uint32_t
kk_vrs_workspace_align(uint32_t value)
{
   return (value + 15u) & ~15u;
}

static inline bool
kk_vrs_workspace_layout_init(uint32_t input_stride, uint32_t samples,
   uint32_t requested_jobs, uint32_t byte_budget, struct kk_vrs_workspace_layout *out)
{
   if (!out) return false;
   memset(out, 0, sizeof(*out));
   if (input_stride < 64 || input_stride % 16 || input_stride > ((UINT16_MAX + 96u + 15u) & ~15u) ||
       (samples != 1 && samples != 2 && samples != 4) ||
       !requested_jobs || requested_jobs > KK_VRS_WORKSPACE_MAX_JOBS ||
       !byte_budget || byte_budget > KK_VRS_WORKSPACE_BUDGET)
      return false;
   for (uint32_t side = 32; side >= 8; side /= 2) {
      uint64_t pixels = side * side, sample_pixels = pixels * samples;
      uint64_t fixed = kk_vrs_workspace_align(sizeof(struct kk_depth_bounds_envelope)) +
         kk_vrs_workspace_align(sizeof(struct kk_vrs_mesh_jobs_state)) + sample_pixels * (8 + 8 * 16);
      uint64_t per_job = side / 8 * sizeof(struct kk_vrs_transaction_context) +
         sizeof(struct kk_mesh_retained_job) + pixels * 24 + sample_pixels * input_stride;
      if (byte_budget <= fixed) continue;
      uint32_t jobs = (uint32_t)((byte_budget - fixed) / per_job);
      if (jobs > requested_jobs) jobs = requested_jobs;
      if (jobs && fixed + per_job * jobs + (jobs % 2 ? 8 : 0) > byte_budget) jobs--;
      if (!jobs) continue;
      struct kk_vrs_workspace_layout layout = {
         .byte_budget = byte_budget, .requested_jobs = requested_jobs,
         .side = side, .rows = side / 8, .jobs = jobs,
         .samples = samples, .input_stride = input_stride,
      };
      uint32_t next = kk_vrs_workspace_align(sizeof(struct kk_depth_bounds_envelope));
      layout.contexts = next;
      next += jobs * layout.rows * sizeof(struct kk_vrs_transaction_context);
      layout.job_records = next;
      next = kk_vrs_workspace_align(next + jobs * sizeof(struct kk_mesh_retained_job));
      layout.cursor = next;
      next += kk_vrs_workspace_align(sizeof(struct kk_vrs_mesh_jobs_state));
      layout.inputs = next;
      next += jobs * sample_pixels * input_stride;
      layout.claims = next; next += jobs * pixels * 4;
      layout.coverage = next; next += jobs * pixels * 4;
      layout.metadata = next; next += jobs * pixels * 16;
      layout.depth = next; next += sample_pixels * 4;
      layout.stencil = next; next += sample_pixels * 4;
      for (unsigned i = 0; i < 8; i++) {
         layout.color[i] = next;
         next += sample_pixels * 16;
      }
      layout.bytes = next;
      *out = layout;
      return true;
   }
   return false;
}
#endif
