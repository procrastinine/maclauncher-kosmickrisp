/* SPDX-License-Identifier: MIT */
#include "mtl_gpu_timing.h"
bool mtl_gpu_timing_enabled;
void *mtl_gpu_timing_configure(mtl_device *d, void *p, mtl_gpu_encoder_callback f) { return 0; }
void mtl_gpu_timing_attach_command(void *d, mtl_command_buffer *c) {}
void mtl_gpu_timing_reset_command(mtl_command_buffer *c) {}
void *mtl_gpu_timing_encoder_begin(mtl_command_buffer *c, bool r, void *d) { return 0; }
void *mtl_gpu_timing_encoder_wrap(void *r, void *e) { return e; }
void *mtl_gpu_timing_snapshot(mtl_command_buffer **c, uint32_t n) { return 0; }
void mtl_gpu_timing_resolve(void *s, bool ok) {}
void *mtl_gpu_timing_external_begin(mtl_command_buffer *c, const char *k, uint32_t w, uint32_t h) { return 0; }
void mtl_gpu_timing_external_end(void *r) {}
void mtl_gpu_timing_library(void *l, const char *s) {}
void mtl_gpu_timing_pipeline(void *p, void *d, bool r) {}

void mtl_gpu_timing_pipeline_kind(void *p, const char *k) {}
void mtl_gpu_timing_meta(mtl_command_buffer *c, bool b) {}
void *mtl_gpu_timing_feedback_prepare(void *o) { return 0; }
void mtl_gpu_timing_feedback_capture(void *o, mtl_command_buffer **c, uint32_t n) {}
