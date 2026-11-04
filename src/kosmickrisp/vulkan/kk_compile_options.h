/* SPDX-License-Identifier: MIT */
#ifndef KK_COMPILE_OPTIONS_H
#define KK_COMPILE_OPTIONS_H

#include <stdbool.h>
#include "kosmickrisp/libkk/kk_argument_table.h"

struct kk_per_vertex_map;
struct nir_to_msl_options;
struct msl_per_vertex_inputs;

struct kk_compile_options {
   const struct kk_per_vertex_map *per_vertex_map;
   const struct msl_per_vertex_inputs *native_per_vertex;
   struct nir_shader *conservative_fs;
   bool independent_interface;
   /* Static last-vertex provoking for a Mesh pipeline whose fragment stage
    * reads flat inputs and no barycentric coordinates or per-vertex inputs. */
   bool mesh_provoking_last;
   /* The same fragment inputs with a dynamic provoking mode: the Mesh shader
    * selects the index order from per-draw data. */
   bool mesh_provoking_dynamic;
   bool poly_layer_replay;
   bool polygon_capture;
   bool polygon_regular;
   bool pixel_envelope;
   bool depth_bounds;
   bool pixel_sample_shading;
   bool mesh_queries;
   bool pipeline_statistics;
   bool geometry_reports;
   uint32_t *mesh_retained_strides;
   uint32_t *mesh_retained_fields;
   const struct nir_to_msl_options *msl_custom;
};

#endif
