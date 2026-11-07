/* SPDX-License-Identifier: MIT */
#ifndef MTL_GPU_TIMING_H
#define MTL_GPU_TIMING_H
#include <stdbool.h>
#include <stdint.h>
#include "mtl_types.h"
#define MTL_GPU_TIMING_HASHES 16
struct mtl_gpu_encoder_result {
   char kind[32];
   char hashes[MTL_GPU_TIMING_HASHES][65];
   uint32_t hash_count, width, height, depth;
   uint64_t draws, dispatches, blits;
   double ms;
   bool valid, extent_known, hashes_truncated, pipeline_unknown;
};
typedef void (*mtl_gpu_encoder_callback)(void *, const struct mtl_gpu_encoder_result *);
extern bool mtl_gpu_timing_enabled;
void *mtl_gpu_timing_configure(mtl_device *, void *, mtl_gpu_encoder_callback);
void mtl_gpu_timing_attach_command(void *, mtl_command_buffer *);
void mtl_gpu_timing_reset_command(mtl_command_buffer *);
void *mtl_gpu_timing_encoder_begin(mtl_command_buffer *, bool render, void *descriptor);
void *mtl_gpu_timing_encoder_wrap(void *record, void *native_encoder);
void *mtl_gpu_timing_snapshot(mtl_command_buffer **, uint32_t count);
void mtl_gpu_timing_resolve(void *snapshot, bool success);
void *mtl_gpu_timing_external_begin(mtl_command_buffer *, const char *, uint32_t, uint32_t);
void mtl_gpu_timing_external_end(void *record);
void mtl_gpu_timing_library(void *library, const char *source);
void mtl_gpu_timing_pipeline_kind(void *, const char *);
void mtl_gpu_timing_meta(mtl_command_buffer *, bool begin);
void *mtl_gpu_timing_feedback_prepare(void *options);
void mtl_gpu_timing_feedback_capture(void *options, mtl_command_buffer **, uint32_t);
void mtl_gpu_timing_pipeline(void *pipeline, void *descriptor, bool render);
#endif
