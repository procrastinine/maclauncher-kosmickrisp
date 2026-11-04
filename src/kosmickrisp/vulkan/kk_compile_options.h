/* SPDX-License-Identifier: MIT */
#ifndef KK_COMPILE_OPTIONS_H
#define KK_COMPILE_OPTIONS_H

#include <stdbool.h>
#include "kosmickrisp/libkk/kk_argument_table.h"

struct kk_per_vertex_map;
struct nir_to_msl_options;

struct kk_compile_options {
   const struct kk_per_vertex_map *per_vertex_map;
   struct nir_shader *conservative_fs;
   struct nir_shader *vrs_source;
   bool independent_interface;
   bool poly_layer_replay;
   bool pixel_envelope;
   bool pixel_sample_shading;
   bool mesh_queries;
   uint32_t *mesh_retained_strides;
   uint32_t *mesh_retained_fields;
   const struct nir_to_msl_options *msl_custom;
};

#endif
