/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SHADER_DATA_H
#define KK_INTERLOCK_SHADER_DATA_H

#include "kk_shader.h"
#include "util/format/u_format.h"
#include "kk_interlock_capture_nir.h"
#include "util/simple_mtx.h"

enum kk_interlock_program {
   KK_INTERLOCK_CAPTURE,
   KK_INTERLOCK_TRANSACTION,
   KK_INTERLOCK_REPLAY,
   KK_INTERLOCK_TILE_VERTEX,
   KK_INTERLOCK_LOAD,
   KK_INTERLOCK_PROGRAM_COUNT,
};

struct kk_interlock_shader_info {
   struct kk_interlock_capture_nir inputs;
   enum pipe_format color_format[8];
   unsigned depth_bits;
   unsigned source_vertices_per_primitive;
   bool depth;
   bool stencil;
   bool framebuffer_reads[KK_INTERLOCK_REPLAY + 1];
};

struct kk_interlock_shader {
   uint32_t references;
   struct kk_interlock_shader_info info;
   struct msl_compile_data program[KK_INTERLOCK_PROGRAM_COUNT];
};

struct kk_interlock_pipeline_entry;
struct kk_interlock_pipeline {
   mtl_render_pipeline_descriptor *descriptor[3];
   bool framebuffer_reads[3];
   bool uses_pipeline_statistics[3];
   mtl_compute_pipeline_state *load;
   mtl_depth_stencil_state *replace;
   struct kk_interlock_pipeline_entry *entries;
   simple_mtx_t mutex;
};

VkResult kk_interlock_pipeline_compile(struct kk_device *dev,
                                       struct kk_shader *shader);
void kk_interlock_pipeline_finish(struct kk_interlock_pipeline *pipeline);
VkResult kk_interlock_pipeline_get(struct kk_device *dev,
   struct kk_interlock_pipeline *pipeline, enum kk_interlock_program program,
   const enum mtl_pixel_format formats[8], unsigned samples,
   enum mtl_primitive_topology_class topology, bool pipeline_statistics,
   mtl_render_pipeline_state **out);

#endif
