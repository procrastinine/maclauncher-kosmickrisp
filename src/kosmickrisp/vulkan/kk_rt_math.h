/*
 * Copyright © 2021 Google
 * Copyright © 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

nir_def *
kk_rt_mul_vec3_mat(nir_builder *b, nir_def *vec, nir_def *matrix[], bool translation)
{
   const unsigned saved_math = b->fp_math_ctrl;
   b->fp_math_ctrl = nir_fp_no_fast_math;
   nir_def *result_components[3] = {
      nir_channel(b, matrix[0], 3),
      nir_channel(b, matrix[1], 3),
      nir_channel(b, matrix[2], 3),
   };
   for (unsigned i = 0; i < 3; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         nir_def *v =
            nir_fmul(b, nir_channels(b, vec, 1 << j), nir_channels(b, matrix[i], 1 << j));
         result_components[i] = (translation || j) ? nir_fadd(b, result_components[i], v) : v;
      }
   }
   nir_def *result = nir_vec(b, result_components, 3);
   b->fp_math_ctrl = saved_math;
   return result;
}

static nir_def *
kk_rt_build_intersect_edge(nir_builder *b, nir_def *v0_x, nir_def *v0_y, nir_def *v1_x, nir_def *v1_y)
{
   /* Test (1 0 0) direction: t = <v1-v0, (1 0 0)> */
   nir_def *t_x = nir_fsub(b, v1_x, v0_x);
   nir_def *test_y = nir_feq_imm(b, t_x, 0.0);
   /* Test (0 1 0) direction: t = <v1-v0, (0 1 0)> */
   nir_def *t_y = nir_fsub(b, v1_y, v0_y);

   return nir_bcsel(b, test_y, nir_flt_imm(b, t_y, 0.0), nir_flt_imm(b, t_x, 0.0));
}

static nir_def *
kk_rt_build_intersect_vertex(nir_builder *b, nir_def *v0_x, nir_def *v1_x, nir_def *v2_x)
{
   /* Choose n=(1 0 0) to simplify the dot product. */
   nir_def *edge0 = nir_fsub(b, v1_x, v0_x);
   nir_def *edge1 = nir_fsub(b, v2_x, v0_x);
   return nir_iand(b, nir_fle_imm(b, edge0, 0.0), nir_fgt_imm(b, edge1, 0.0));
}

static nir_def *
kk_rt_build_intersect_ray_tri(nir_builder *b, nir_def **node_data, nir_def *ray_tmax,
                            nir_def *origin, nir_def *dir, nir_def *inv_dir)
{
   const struct glsl_type *vec4_type = glsl_vector_type(GLSL_TYPE_FLOAT, 4);

   const uint32_t coord_offsets[3] = {
      0,
      12,
      24,
   };

   nir_def *node_coords[3] = {
      nir_vec3(b,
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[0] + 0),
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[0] + 4),
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[0] + 8)
      ),
      nir_vec3(b,
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[1] + 0),
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[1] + 4),
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[1] + 8)
      ),
      nir_vec3(b,
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[2] + 0),
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[2] + 4),
         kk_rt_load_node_data(b, NULL, node_data, coord_offsets[2] + 8)
      ),
   };

   nir_variable *result = nir_variable_create(b->shader, nir_var_shader_temp, vec4_type, "result");
   nir_store_var(b, result, nir_imm_vec4(b, INFINITY, 1.0f, 0.0f, 0.0f), 0xf);

   /* Based on watertight Ray/Triangle intersection from
    * http://jcgt.org/published/0002/01/05/paper.pdf */

   /* Calculate the dimension where the ray direction is largest */
   nir_def *abs_dir = nir_fabs(b, dir);

   nir_def *abs_dirs[3] = {
      nir_channel(b, abs_dir, 0),
      nir_channel(b, abs_dir, 1),
      nir_channel(b, abs_dir, 2),
   };
   /* Find index of greatest value of abs_dir and put that as kz. */
   nir_def *kz = nir_bcsel(
      b, nir_fge(b, abs_dirs[0], abs_dirs[1]),
      nir_bcsel(b, nir_fge(b, abs_dirs[0], abs_dirs[2]), nir_imm_int(b, 0), nir_imm_int(b, 2)),
      nir_bcsel(b, nir_fge(b, abs_dirs[1], abs_dirs[2]), nir_imm_int(b, 1), nir_imm_int(b, 2)));
   nir_def *kx = nir_imod_imm(b, nir_iadd_imm(b, kz, 1), 3);
   nir_def *ky = nir_imod_imm(b, nir_iadd_imm(b, kx, 1), 3);
   nir_def *k_indices[3] = {kx, ky, kz};
   nir_def *k = nir_vec(b, k_indices, 3);

   /* Swap kx and ky dimensions to preserve winding order */
   unsigned swap_xy_swizzle[4] = {1, 0, 2, 3};
   k = nir_bcsel(b, nir_flt_imm(b, nir_vector_extract(b, dir, kz), 0.0f),
                 nir_swizzle(b, k, swap_xy_swizzle, 3), k);

   kx = nir_channel(b, k, 0);
   ky = nir_channel(b, k, 1);
   kz = nir_channel(b, k, 2);

   /* Calculate shear constants */
   nir_def *sz = nir_frcp(b, nir_vector_extract(b, dir, kz));
   nir_def *sx = nir_fmul(b, nir_vector_extract(b, dir, kx), sz);
   nir_def *sy = nir_fmul(b, nir_vector_extract(b, dir, ky), sz);

   /* Calculate vertices relative to ray origin */
   nir_def *v_a = nir_fsub(b, node_coords[0], origin);
   nir_def *v_b = nir_fsub(b, node_coords[1], origin);
   nir_def *v_c = nir_fsub(b, node_coords[2], origin);

   /* Perform shear and scale */
   nir_def *ax =
      nir_fsub(b, nir_vector_extract(b, v_a, kx), nir_fmul(b, sx, nir_vector_extract(b, v_a, kz)));
   nir_def *ay =
      nir_fsub(b, nir_vector_extract(b, v_a, ky), nir_fmul(b, sy, nir_vector_extract(b, v_a, kz)));
   nir_def *bx =
      nir_fsub(b, nir_vector_extract(b, v_b, kx), nir_fmul(b, sx, nir_vector_extract(b, v_b, kz)));
   nir_def *by =
      nir_fsub(b, nir_vector_extract(b, v_b, ky), nir_fmul(b, sy, nir_vector_extract(b, v_b, kz)));
   nir_def *cx =
      nir_fsub(b, nir_vector_extract(b, v_c, kx), nir_fmul(b, sx, nir_vector_extract(b, v_c, kz)));
   nir_def *cy =
      nir_fsub(b, nir_vector_extract(b, v_c, ky), nir_fmul(b, sy, nir_vector_extract(b, v_c, kz)));

   nir_def *u = nir_fsub(b, nir_fmul(b, cx, by), nir_fmul(b, cy, bx));
   nir_def *v = nir_fsub(b, nir_fmul(b, ax, cy), nir_fmul(b, ay, cx));
   nir_def *w = nir_fsub(b, nir_fmul(b, bx, ay), nir_fmul(b, by, ax));

   /* Perform edge tests. */
   nir_def *cond_back = nir_ior(b, nir_ior(b, nir_flt_imm(b, u, 0.0f), nir_flt_imm(b, v, 0.0f)),
                                    nir_flt_imm(b, w, 0.0f));

   nir_def *cond_front = nir_ior(
      b, nir_ior(b, nir_fgt_imm(b, u, 0.0f), nir_fgt_imm(b, v, 0.0f)), nir_fgt_imm(b, w, 0.0f));

   nir_def *cond = nir_inot(b, nir_iand(b, cond_back, cond_front));

   /* When an edge is hit, we have to ensure that it is not hit twice in case it is shared.
    *
    * Vulkan 1.4.322, Section 40.1.1 Watertightness:
    *
    *    Any set of two triangles with two shared vertices that were specified in the same
    *    winding order in each triangle have a shared edge defined by those vertices.
    *
    * This means we can decide which triangle should intersect by comparing the shared edge
    * to two arbitrary directions because the shared edges are antiparallel. The triangle
    * vertices are transformed so the ray direction is (0 0 1). Therefore it makes sense to
    * choose (1 0 0) and (0 1 0) as reference directions.
    *
    * Hitting edges is extremely rare so an if should be worth.
    */
   nir_def *is_edge_a = nir_feq_imm(b, u, 0.0f);
   nir_def *is_edge_b = nir_feq_imm(b, v, 0.0f);
   nir_def *is_edge_c = nir_feq_imm(b, w, 0.0f);
   nir_def *cond_edge = nir_ior(b, is_edge_a, nir_ior(b, is_edge_b, is_edge_c));
   nir_def *intersect_edge = cond;
   nir_push_if(b, cond_edge);
   {
      nir_def *intersect_edge_a = nir_iand(b, is_edge_a, kk_rt_build_intersect_edge(b, bx, by, cx, cy));
      nir_def *intersect_edge_b = nir_iand(b, is_edge_b, kk_rt_build_intersect_edge(b, cx, cy, ax, ay));
      nir_def *intersect_edge_c = nir_iand(b, is_edge_c, kk_rt_build_intersect_edge(b, ax, ay, bx, by));
      intersect_edge = nir_iand(b, intersect_edge, nir_ior(b, nir_ior(b, intersect_edge_a, intersect_edge_b), intersect_edge_c));

      /* For vertices, special handling is needed to avoid double hits. The spec defines
       * shared vertices as follows (Vulkan 1.4.322, Section 40.1.1 Watertightness):
       *
       *    Any set of two or more triangles where all triangles have one vertex with an
       *    identical position value, that vertex is a shared vertex.
       *
       * Since the no double hit/miss requirement of a shared vertex is only formulated for
       * closed fans
       *
       *    Implementations should not double-hit or miss when a ray intersects a shared edge,
       *    or a shared vertex of a closed fan.
       *
       * it is possible to choose an arbitrary direction n that defines which triangle in the
       * closed fan should intersect the shared vertex with the ray.
       *
       *    All edges that include the above vertex are shared edges.
       *
       * Implies that all triangles have the same winding order. It is therefore sufficiant
       * to choose the triangle where the other vertices are on both sides of a plane
       * perpendicular to n (relying on winding order to get one instead of two triangles
       * that meet said condition).
       */
      nir_def *is_vertex_a = nir_iand(b, is_edge_b, is_edge_c);
      nir_def *is_vertex_b = nir_iand(b, is_edge_a, is_edge_c);
      nir_def *is_vertex_c = nir_iand(b, is_edge_a, is_edge_b);
      nir_def *intersect_vertex_a = nir_iand(b, is_vertex_a, kk_rt_build_intersect_vertex(b, ax, bx, cx));
      nir_def *intersect_vertex_b = nir_iand(b, is_vertex_b, kk_rt_build_intersect_vertex(b, bx, cx, ax));
      nir_def *intersect_vertex_c = nir_iand(b, is_vertex_c, kk_rt_build_intersect_vertex(b, cx, ax, bx));
      nir_def *is_vertex = nir_ior(b, nir_ior(b, is_vertex_a, is_vertex_b), is_vertex_c);
      nir_def *intersect_vertex = nir_ior(b, nir_ior(b, intersect_vertex_a, intersect_vertex_b), intersect_vertex_c);
      intersect_vertex = nir_ior(b, nir_inot(b, is_vertex), intersect_vertex);
      intersect_edge = nir_iand(b, intersect_edge, intersect_vertex);
   }
   nir_pop_if(b, NULL);
   cond = nir_if_phi(b, intersect_edge, cond);

   nir_push_if(b, cond);
   {
      nir_def *det = nir_fadd(b, u, nir_fadd(b, v, w));

      nir_def *az = nir_fmul(b, sz, nir_vector_extract(b, v_a, kz));
      nir_def *bz = nir_fmul(b, sz, nir_vector_extract(b, v_b, kz));
      nir_def *cz = nir_fmul(b, sz, nir_vector_extract(b, v_c, kz));

      nir_def *t =
         nir_fadd(b, nir_fadd(b, nir_fmul(b, u, az), nir_fmul(b, v, bz)), nir_fmul(b, w, cz));

      nir_def *t_signed = nir_fmul(b, nir_fsign(b, det), t);

      nir_def *det_cond_front = nir_inot(b, nir_flt_imm(b, t_signed, 0.0f));

      nir_push_if(b, det_cond_front);
      {
         t = nir_fdiv(b, t, det);
         v = nir_fdiv(b, v, det);
         w = nir_fdiv(b, w, det);

         nir_def *indices[4] = {t, det, v, w};
         nir_store_var(b, result, nir_vec(b, indices, 4), 0xf);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);

   return nir_load_var(b, result);
}

