/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_METAL_H
#define KK_RT_METAL_H
#include "kosmickrisp/bridge/mtl_types.h"
#include <stdint.h>
#include <stdbool.h>

/* Final native objects belong to one common vk_rt_pipeline. Function libraries
 * and descriptors belong to their common vk_shader objects, not this wrapper. */
struct kk_rt_native_pipeline {
   mtl_compute_pipeline_state *pipeline;
   mtl_allocation *function_table;
   uint64_t function_table_id;
};
bool kk_rt_link_functions(mtl_compiler *compiler,
                          mtl_function_descriptor *dispatcher,
                          uint32_t function_count,
                          mtl_function_descriptor *const *functions,
                          uint32_t max_threads,
                          struct kk_rt_native_pipeline *out);
void kk_rt_native_pipeline_finish(struct kk_rt_native_pipeline *pipeline);
#endif
