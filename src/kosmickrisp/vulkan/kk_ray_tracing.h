/*
 * Copyright 2021 Google
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#ifndef KK_RAY_TRACING_H
#define KK_RAY_TRACING_H
#include "nir/nir.h"
#include "nir/nir_builder.h"

struct kk_rt_ray_traversal_args;
struct kk_rt_ray_flags {
   nir_def *force_opaque, *force_not_opaque, *terminate_on_first_hit;
   nir_def *no_cull_front, *no_cull_back, *no_cull_opaque, *no_cull_no_opaque;
   nir_def *no_skip_triangles, *no_skip_aabbs;
};
struct kk_rt_leaf_intersection {
   nir_def *node_addr;
   nir_def *primitive_id;
   nir_def *geometry_id_and_flags;
   nir_def *opaque;
};
struct kk_rt_triangle_intersection {
   struct kk_rt_leaf_intersection base;
   nir_def *t, *frontface, *barycentrics;
};
typedef void (*kk_rt_aabb_intersection_cb)(nir_builder *, struct kk_rt_leaf_intersection *,
   const struct kk_rt_ray_traversal_args *, const struct kk_rt_ray_flags *);
typedef void (*kk_rt_triangle_intersection_cb)(nir_builder *, struct kk_rt_triangle_intersection *,
   const struct kk_rt_ray_traversal_args *, const struct kk_rt_ray_flags *);

/* Explicit invocation-private state is spillable across RT shader calls. Node
 * and successor offsets remain64-bit; one saved TLAS successor replaces a fixed
 * traversal stack. User ray recursion has its own continuation frames. */
struct kk_rt_ray_traversal_vars {
   nir_deref_instr *tmax, *origin, *dir, *inv_dir;
   nir_deref_instr *bvh_base, *current_node;
   nir_deref_instr *tlas_resume, *in_blas;
   nir_deref_instr *instance_addr, *instance_payload_addr, *sbt_offset_and_flags;
};
struct kk_rt_ray_traversal_args {
   nir_def *root_bvh_base, *flags, *cull_mask, *origin, *tmin, *dir;
   struct kk_rt_ray_traversal_vars vars;
   kk_rt_aabb_intersection_cb aabb_cb;
   kk_rt_triangle_intersection_cb triangle_cb;
   void *data;
};

nir_def *kk_rt_mul_vec3_mat(nir_builder *, nir_def *, nir_def *[], bool);
void kk_rt_load_wto_matrix(nir_builder *, nir_def *, nir_def **, nir_def **);
nir_def *kk_rt_load_vertex_position(nir_builder *, nir_def *, uint32_t);
void kk_rt_begin_traversal(nir_builder *, const struct kk_rt_ray_traversal_args *);
nir_def *kk_rt_build_ray_traversal(nir_builder *, const struct kk_rt_ray_traversal_args *);
bool kk_nir_lower_ray_queries(nir_shader *shader);
bool kk_nir_lower_hit_objects(nir_shader *shader);
#endif
