/* SPDX-License-Identifier: MIT */
#ifndef KK_COMPILE_OPTIONS_H
#define KK_COMPILE_OPTIONS_H

#include <stdbool.h>

struct kk_per_vertex_map;
struct nir_to_msl_options;

struct kk_compile_options {
   const struct kk_per_vertex_map *per_vertex_map;
   bool independent_interface;
   bool mesh_queries;
   const struct nir_to_msl_options *msl_custom;
};

#endif
