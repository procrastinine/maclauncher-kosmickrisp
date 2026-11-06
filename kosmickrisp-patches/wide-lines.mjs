// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-lines-nir.h',
  'kosmickrisp-patches/kosmickrisp-line-dashes.h', 'kosmickrisp-patches/kosmickrisp-line-dashes.cl'];
// Wide lines reuse the polygon capture, clipping and rasterization programs.
export function apply({edit, edits}) {
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .fillModeNonSolid = true,', '      .fillModeNonSolid = true,\n      .wideLines = true,')
    .replace('      .bresenhamLines = true,', '      .bresenhamLines = true,\n      .rectangularLines = true,')
    .replace('      .nonStrictWideLinesUseParallelogram = false,', '      .nonStrictWideLinesUseParallelogram = true,')
    .replace('      .lineWidthRange = {1.0f, 1.0f},', '      .lineWidthRange = {1.0f, 4096.0f},')
    .replace('      .lineWidthGranularity = 0.0f,', '      .lineWidthGranularity = 1.0f / 256.0f,'));
  edit('src/poly/geometry_capture.h', s => s
    .replace('   uint32_t polygon_mode;', `   uint32_t polygon_mode;
   uint32_t primitive_vertices;
   uint32_t line_expand;
   uint32_t line_mode;
   float line_width;`)
    .replace('   uint32_t vertices_out;\n   uint32_t streams;\n   uint8_t clip_count;',
      '   uint32_t vertices_out;\n   uint32_t streams;\n   uint32_t primitive_vertices;\n   uint8_t clip_count;'));
  edit('src/poly/nir/poly_nir_capture_gs.h', s => s
    .replace('      nir_push_if(b, nir_uge_imm(b, id, 2));', `      unsigned primitive_vertices =
         u_reduced_prim(b->shader->info.gs.output_primitive) == MESA_PRIM_LINES ? 2 : 3;
      nir_push_if(b, nir_uge_imm(b, id, primitive_vertices - 1));`)
    .replace('         nir_def *primitive = nir_iadd_imm(b, id, -2);', `         nir_def *primitive = nir_iadd_imm(b, id, 1 - primitive_vertices);
         if (primitive_vertices == 2) {
            nir_def *base = nir_iadd(b,
               nir_imul(b, calc_unrolled_id(b), CAPTURE_PARAM(b, streams)),
               nir_imm_int(b, nir_intrinsic_stream_id(intr)));
            nir_def *index = nir_iadd(b,
               nir_imul(b, base, nir_iadd_imm(b, CAPTURE_PARAM(b, vertices_out), -1)),
               nir_iadd(b, intr->src[3].ssa, primitive));
            nir_def *address = nir_iadd(b, CAPTURE_PARAM(b, triangles),
               nir_u2u64(b, nir_imul_imm(b, index, 16)));
            nir_def *a = nir_iadd(b, first, primitive);
            nir_def *c = nir_iadd_imm(b, a, 1);
            nir_store_global(b, nir_vec4(b, a, c, c, nir_imm_int(b, 0)), address);
         } else {`)
    .replace('            nir_iadd(b, first, c), nir_imm_int(b, 0)), triangle_address);',
      '            nir_iadd(b, first, c), nir_imm_int(b, 0)), triangle_address);\n         }')
    .replace('      .vertices_out = gs->info.gs.vertices_out,', `      .vertices_out = gs->info.gs.vertices_out,
      .primitive_vertices = u_reduced_prim(gs->info.gs.output_primitive) == MESA_PRIM_LINES ? 2 : 3,`));
  edit('src/kosmickrisp/libkk/kk_polygon.h', s => s.replace(
    '/* Perspective varyings interpolate with homogeneous clipping weights.', `/* Line clipping shares the polygon plane distances, original-vertex weights
 * and intersection interpolation. Cull distances reject an entire segment. */
static inline unsigned
kk_polygon_clip_line(const float position[3][4],
                     const float clip[3][KK_POLYGON_CLIP_DISTANCES],
                     const float cull[3][KK_POLYGON_CLIP_DISTANCES],
                     unsigned clip_count, unsigned cull_count,
                     bool depth_clip, bool negative_one_to_one,
                     struct kk_polygon_vertex out[KK_POLYGON_MAX_VERTICES])
{
   for (unsigned i = 0; i < cull_count; ++i)
      if (cull[0][i] < 0 && cull[1][i] < 0) return 0;
   for (unsigned v = 0; v < 2; ++v) {
      for (unsigned c = 0; c < 4; ++c) out[v].position[c] = position[v][c];
      for (unsigned c = 0; c < 3; ++c) out[v].weight[c] = v == c ? 1.0f : 0.0f;
   }
   for (unsigned plane = 0; plane < 6 + clip_count; ++plane) {
      if (!depth_clip && (plane == 4 || plane == 5)) continue;
      float a = kk_polygon_plane_distance(&out[0], plane, clip, negative_one_to_one);
      float b = kk_polygon_plane_distance(&out[1], plane, clip, negative_one_to_one);
      if (a < 0 && b < 0) return 0;
      if ((a < 0) != (b < 0)) {
         struct kk_polygon_vertex intersection =
            kk_polygon_intersection(&out[0], &out[1], a / (a - b));
         out[a < 0 ? 0 : 1] = intersection;
      }
   }
   return 2;
}

/* Perspective varyings interpolate with homogeneous clipping weights.`));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('uint triangles = streams * sub_sat(p->vertices_out, 2u);',
      'uint triangles = streams * sub_sat(p->vertices_out, p->primitive_vertices - 1u);')
    .replace('uint max_primitives = sub_sat(p->vertices_out, 2u);',
      'uint max_primitives = sub_sat(p->vertices_out, p->primitive_vertices - 1u);')
    .replace(`   uint count = kk_polygon_clip(positions, clip, cull, p->clip_count, p->cull_count,
                                p->depth_clip, p->negative_one_to_one, output);
   if (count < 3)`, `   uint count = p->primitive_vertices == 2 ?
      kk_polygon_clip_line(positions, clip, cull, p->clip_count, p->cull_count,
                           p->depth_clip, p->negative_one_to_one, output) :
      kk_polygon_clip(positions, clip, cull, p->clip_count, p->cull_count,
                      p->depth_clip, p->negative_one_to_one, output);
   if (count < (p->primitive_vertices == 2 ? 2u : 3u))`)
    .replace('   uint provoking = p->provoking_last ? 2 : 0;',
      '   uint provoking = p->provoking_last ? p->primitive_vertices - 1 : 0;')
    .replace('   bool front = p->front_ccw ? area > 0 : area < 0;',
      '   bool front = p->primitive_vertices == 2 || (p->front_ccw ? area > 0 : area < 0);')
    .replace('   if (!defer_facing && ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2))))',
      '   if (p->primitive_vertices != 2 && !defer_facing &&\n       ((front && (p->cull_mode & 1)) || (!front && (p->cull_mode & 2))))')
    .replace('   counts[id] = p->polygon_mode == 2 ? count :',
      '   counts[id] = p->primitive_vertices == 2 ? count :\n      p->polygon_mode == 2 ? count :')
    .replace('   p->draw[0] = count * (p->polygon_mode == 2 ? 6 : 1);',
      `   uint line_vertices = p->line_mode == 2 ? 2u * (uint)(p->line_width + 0.5f) : 6u;
   p->draw[0] = p->line_expand ? count / 2u * line_vertices :
      count * (p->polygon_mode == 2 ? 6 : 1);`)
    .replace('   uint count = p->polygon_mode == 2 ? output_count :',
      '   uint count = p->primitive_vertices == 2 ? output_count :\n      p->polygon_mode == 2 ? output_count :')
    .replace('   uint provoking = p->provoking_last ? 2 : 0;',
      '   uint provoking = p->provoking_last ? p->primitive_vertices - 1 : 0;')
    .replace('   float bias = polygon_depth_bias(p, viewport, polygon, count);',
      '   float bias = p->primitive_vertices == 2 ? 0 : polygon_depth_bias(p, viewport, polygon, count);')
    .replace('      uint vertex_index = p->polygon_mode == 2 ? i :',
      '      uint vertex_index = p->primitive_vertices == 2 ? i :\n         p->polygon_mode == 2 ? i :'));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('                          const nir_shader_compiler_options *options)\n{',
      '                          const nir_shader_compiler_options *options, bool line_effects)\n{')
    .replace('   nir->info.outputs_written = (info->outputs & ~VARYING_BIT_PSIZ) | VARYING_BIT_PNTC;',
      '   nir->info.outputs_written = (info->outputs & ~VARYING_BIT_PSIZ) | VARYING_BIT_PNTC;\n   if (line_effects) nir->info.outputs_written |= VARYING_BIT_FOGC;')
    .replace('static bool\nkk_polygon_regular_possible(', `static bool
kk_wide_line_possible(const struct vk_graphics_pipeline_state *state)
{
   if (!state || !state->rs) return false;
   bool possible = state->rs->line.width != 1.0f ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_WIDTH) ||
      state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE);
   bool line_class = state->rs->polygon_mode == VK_POLYGON_MODE_LINE ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_POLYGON_MODE) ||
      (state->ia && u_reduced_prim(vk_topology_to_mesa(state->ia->primitive_topology)) == MESA_PRIM_LINES) ||
      (state->shader_stages & (VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT));
   return possible && line_class;
}

static bool
kk_polygon_regular_possible(`)
    .replace('      (state->rs->polygon_mode == VK_POLYGON_MODE_POINT ||',
      '      (kk_wide_line_possible(state) || state->rs->polygon_mode == VK_POLYGON_MODE_POINT ||')
    .replace('   nir_def *point = nir_bcsel(&b, is_point, nir_udiv_imm(&b, id, 6), id);',
      `   nir_def *line_expand = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, line_expand), 4);
   nir_def *is_line = nir_ine_imm(&b, line_expand, 0);
   nir_def *line_mode = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, line_mode), 4);
   nir_def *bresenham = nir_iand(&b, is_line, nir_ieq_imm(&b, line_mode, 2));
   nir_def *line_width = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, line_width), 4);
   nir_def *copies = nir_f2u32(&b, nir_ffloor(&b, nir_fadd_imm(&b, line_width, 0.5f)));
   nir_def *line_vertices = nir_bcsel(&b, bresenham,
      nir_imul_imm(&b, copies, 2), nir_imm_int(&b, 6));
   nir_def *segment = nir_udiv(&b, id, line_vertices);
   nir_def *local_id = nir_umod(&b, id, line_vertices);
   nir_def *default_front = nir_imm_int(&b, 1);
   nir_push_if(&b, is_line);
   nir_def *segment_address = nir_iadd(&b,
      kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
      nir_u2u64(&b, nir_iadd_imm(&b,
         nir_imul_imm(&b, segment, 2 * (util_bitcount64(info->outputs) + 1) * 16),
         util_bitcount64(info->outputs) * 16)));
   nir_def *segment_front = nir_load_global_constant(&b, 1, 32, segment_address, .align_mul = 16);
   nir_pop_if(&b, NULL);
   nir_def *line_front = nir_if_phi(&b, segment_front, default_front);
   nir_def *line_ccw = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, front_ccw), 4);
   nir_def *line_order = nir_bcsel(&b, nir_ine(&b, line_front, line_ccw), local_id,
      nir_iadd(&b, nir_imul_imm(&b, nir_udiv_imm(&b, local_id, 3), 3),
         nir_bcsel(&b, nir_ieq_imm(&b, nir_umod_imm(&b, local_id, 3), 0),
            nir_imm_int(&b, 0),
            nir_isub(&b, nir_imm_int(&b, 3), nir_umod_imm(&b, local_id, 3)))));
   const unsigned line_corners[6] = {0, 2, 3, 0, 3, 1};
   nir_def *line_corner = nir_imm_int(&b, 0);
   for (unsigned i = 0; i < ARRAY_SIZE(line_corners); ++i)
      line_corner = nir_bcsel(&b, nir_ieq_imm(&b, line_order, i),
         nir_imm_int(&b, line_corners[i]), line_corner);
   nir_def *endpoint = nir_bcsel(&b, bresenham, nir_iand_imm(&b, id, 1),
      nir_ushr_imm(&b, line_corner, 1));
   nir_def *point = nir_bcsel(&b, is_line,
      nir_iadd(&b, nir_imul_imm(&b, segment, 2), endpoint),
      nir_bcsel(&b, is_point, nir_udiv_imm(&b, id, 6), id));`)
    .replace(`         screen = nir_fadd(&b, screen, nir_bcsel(&b, is_point,
            nir_fadd_imm(&b, coord, -0.5f), nir_imm_vec2(&b, 0, 0)));`,
      `         nir_def *offset = nir_bcsel(&b, is_point,
            nir_fadd_imm(&b, coord, -0.5f), nir_imm_vec2(&b, 0, 0));
         nir_def *plain_screen = nir_fadd(&b, screen, offset);
         nir_push_if(&b, is_line);
         {
            nir_def *other_index = nir_iadd(&b, nir_imul_imm(&b, segment, 2),
               nir_isub(&b, nir_imm_int(&b, 1), endpoint));
            nir_def *other_address = nir_iadd(&b,
               kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
               nir_u2u64(&b, nir_iadd_imm(&b,
                  nir_imul_imm(&b, other_index, (slots + 1) * 16),
                  util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_POS)) * 16)));
            nir_def *other = nir_load_global_constant(&b, 4, 32, other_address, .align_mul = 16);
            nir_def *delta = nir_fmul(&b,
               nir_fsub(&b, nir_fdiv(&b, nir_channels(&b, other, 3), nir_channel(&b, other, 3)),
                  nir_channels(&b, ndc, 3)), nir_fmul_imm(&b, nir_channels(&b, vp, 12), 0.5f));
            delta = nir_bcsel(&b, nir_ieq_imm(&b, endpoint, 0), delta, nir_fneg(&b, delta));
            nir_def *dx = nir_channel(&b, delta, 0), *dy = nir_channel(&b, delta, 1);
            nir_def *length2 = nir_fadd(&b, nir_fmul(&b, dx, dx), nir_fmul(&b, dy, dy));
            nir_def *perpendicular = nir_fmul(&b, nir_vec2(&b, nir_fneg(&b, dy), dx),
               nir_frsq(&b, nir_fmax(&b, length2, nir_imm_float(&b, 1.0e-30f))));
            nir_def *side = nir_fadd_imm(&b,
               nir_u2f32(&b, nir_iand_imm(&b, line_corner, 1)), -0.5f);
            nir_def *rectangle_offset = nir_fmul(&b, perpendicular, nir_fmul(&b, side, line_width));
            nir_def *copy = nir_udiv_imm(&b, local_id, 2);
            nir_def *replica_offset = nir_fsub(&b, nir_u2f32(&b, copy),
               nir_fmul_imm(&b, nir_u2f32(&b, nir_iadd_imm(&b, copies, -1)), 0.5f));
            nir_def *x_major = nir_fge(&b, nir_fabs(&b, dx), nir_fabs(&b, dy));
            nir_def *bresenham_offset = nir_bcsel(&b, x_major,
               nir_vec2(&b, nir_imm_float(&b, 0), replica_offset),
               nir_vec2(&b, replica_offset, nir_imm_float(&b, 0)));
            nir_def *expanded = nir_fadd(&b, screen,
               nir_bcsel(&b, bresenham, bresenham_offset, rectangle_offset));
            nir_pop_if(&b, NULL);
            screen = nir_if_phi(&b, expanded, plain_screen);
         }`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '         u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_TRIANGLES;',
    `         (u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_TRIANGLES ||
          u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_LINES);`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('         .vertices_out = capture->vertices_out,', `         .vertices_out = capture->vertices_out,
         .primitive_vertices = capture->primitive_vertices,
         .line_expand = capture->primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE,
         .line_mode = dyn->rs.line.mode,
         .line_width = CLAMP(floorf(dyn->rs.line.width * 256.0f + 0.5f) / 256.0f, 1.0f, 4096.0f),`)
    .replace('         .cull_mode = dyn->rs.cull_mode,',
      '         .cull_mode = capture->primitive_vertices == 2 ? 0 : dyn->rs.cull_mode,')
    .replace('      (dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias));',
      `      (dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias ||
       ((vs->info.polygon.primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE) &&
        (dyn->rs.line.width != 1.0f || dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR))));`));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s
    .replace('   if (dyn->rs.rasterizer_discard_enable || gfx->is_cull_front_and_back)',
      '   if (dyn->rs.rasterizer_discard_enable ||\n       (gfx->is_cull_front_and_back && vs->info.polygon.primitive_vertices != 2))')
    .replace(`      .primitive_type = dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE ?
         MTL_PRIMITIVE_TYPE_LINE : MTL_PRIMITIVE_TYPE_TRIANGLE,`,
      `      .primitive_type =
         (vs->info.polygon.primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE) &&
         dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM ?
         MTL_PRIMITIVE_TYPE_LINE : MTL_PRIMITIVE_TYPE_TRIANGLE,`));
  nativePassthrough({edit});
  smoothStipple({edit, edits});
  pureEarlyStipple({edit});
  sideEffectEarlyStipple({edit});
  sampleBoundaryStipple({edit});
}

/** Native passthrough shaders are linked before synthetic GS transport can
 * move position components or varying expressions into that stage. The NIR
 * pair exists only during pipeline compilation and is freed by compilation. */
export function nativePassthrough({edit}) {
  edit('src/kosmickrisp/compiler/msl_nir_lower_common.c', s => s.replace(
    '      if (io.location == VARYING_SLOT_LAYER ||\n          io.location == VARYING_SLOT_VIEWPORT) {',
    `      if (io.location == VARYING_SLOT_POS || io.location == VARYING_SLOT_PSIZ ||
          io.location == VARYING_SLOT_CLIP_DIST0 || io.location == VARYING_SLOT_CLIP_DIST1 ||
          io.location == VARYING_SLOT_CULL_DIST0 || io.location == VARYING_SLOT_CULL_DIST1) {
         nir_intrinsic_set_src_type(intr, nir_type_float32);
         return true;
      }
      if (io.location == VARYING_SLOT_LAYER ||
          io.location == VARYING_SLOT_VIEWPORT) {`));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
    .replace('   bool polygon_point;', '   bool polygon_point;\n   bool polygon_native;')
    .replace('   struct msl_compile_data polygon[6];',
      '   struct msl_compile_data polygon[6];\n   struct msl_compile_data polygon_native;\n   struct msl_compile_data polygon_native_fs;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('   uint32_t total_shaders = shader_count;', `   uint32_t total_shaders = shader_count;
   nir_shader *native_pair[2] = {NULL, NULL};
   bool native_passthrough = !explicit_geometry && !xfb && !tess &&
      kk_wide_line_possible(state);
   nir_shader *native_vs = NULL, *native_fs = NULL;
   for (unsigned i = 0; i < shader_count; ++i) {
      nir_shader *n = nir_shaders[i];
      if (n->info.stage == MESA_SHADER_VERTEX) native_vs = n;
      if (n->info.stage == MESA_SHADER_FRAGMENT) {
         native_fs = n;
         if (BITSET_TEST(n->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID))
            native_passthrough = false;
      }
   }
   if (native_passthrough && native_vs) {
      native_pair[0] = nir_shader_clone(NULL, native_vs);
      native_pair[1] = native_fs ? nir_shader_clone(NULL, native_fs) :
         get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);
      native_pair[0]->info.next_stage = MESA_SHADER_FRAGMENT;
      kk_lower_hw_vs(native_pair[0], state, false);
      msl_nir_lower_clip_cull_distance(native_pair[0], 0);
      kk_lower_vs_vbo(native_pair[0], state, vertex_robustness);
      NIR_PASS(_, native_pair[0], msl_nir_lower_instance_id);
      /* The first descriptor pass rewrites uses but keeps dead resource
       * descriptors until DCE. Remove those before lowering only new sysvals. */
      NIR_PASS(_, native_pair[0], nir_opt_dce);
      NIR_PASS(_, native_pair[0], kk_nir_lower_descriptors, vertex_robustness, 0, NULL);
      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
      nir_opt_varyings_bulk(native_pair, 2, true, UINT32_MAX, UINT32_MAX, nir_opts, NULL);
   }`)
    .replace('   const struct msl_compile_data *vs_data = polygon ? &vs->polygon[2 * polygon] : &vs->msl_data[vs_stage];',
      `   const struct msl_compile_data *vs_data = polygon ? &vs->polygon[2 * polygon] :
      vs->info.polygon_native ? &vs->polygon_native : &vs->msl_data[vs_stage];`)
    .replace('   const struct msl_compile_data *fs_data = polygon ? &vs->polygon[2 * polygon + 1] : &vs->msl_data[MESA_SHADER_FRAGMENT];',
      `   const struct msl_compile_data *fs_data = polygon ? &vs->polygon[2 * polygon + 1] :
      vs->info.polygon_native ? &vs->polygon_native_fs : &vs->msl_data[MESA_SHADER_FRAGMENT];`)
    .replace('      gather_graphics_pipeline_create_info(state, shaders, total_shaders);',
      `      gather_graphics_pipeline_create_info(state, shaders, total_shaders);
      if (native_pair[0]) {
         kk_compile_msl(pdev, native_pair[0], state, &shaders[0]->polygon_native);
         kk_compile_msl(pdev, native_pair[1], state, &shaders[0]->polygon_native_fs);
         native_pair[0] = native_pair[1] = NULL;
         shaders[0]->info.polygon_native = true;
      }`)
    .replaceAll('      /* Memset the output array */',
      `      ralloc_free(native_pair[0]);
      ralloc_free(native_pair[1]);
      /* Memset the output array */`)
    .replace('   ralloc_free((void *)shader->gs_main.entrypoint_name);',
      `   ralloc_free((void *)shader->polygon_native.entrypoint_name);
   ralloc_free((void *)shader->polygon_native.code);
   ralloc_free((void *)shader->polygon_native_fs.entrypoint_name);
   ralloc_free((void *)shader->polygon_native_fs.code);
   ralloc_free((void *)shader->gs_main.entrypoint_name);`)
    .replace('   return !blob->out_of_memory;',
      `   if (shader->info.polygon_native) {
      kk_msl_serialize(&shader->polygon_native, blob);
      kk_msl_serialize(&shader->polygon_native_fs, blob);
   }
   return !blob->out_of_memory;`)
    .replace('   if (info.stage == MESA_SHADER_COMPUTE) {',
      `   if (info.polygon_native) {
      result = kk_msl_deserialize(blob, &shader->polygon_native);
      if (result != VK_SUCCESS) goto fail;
      result = kk_msl_deserialize(blob, &shader->polygon_native_fs);
      if (result != VK_SUCCESS) goto fail;
   }
   if (info.stage == MESA_SHADER_COMPUTE) {`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('/* Synthetic XFB passthrough stages are retained by the VS pipeline. */',
      `/* All polygon representations share this selection, including a native
 * passthrough when the original pipeline needs no tessellation, GS or XFB. */
static bool
kk_polygon_expansion_active(struct kk_cmd_buffer *cmd)
{
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   if (!vs || !vs->info.polygon_point) return false;
   bool controlled_bias = dyn->rs.depth_bias.enable &&
      (dyn->rs.depth_bias.exact || dyn->rs.depth_bias.representation !=
         VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT);
   bool lines = vs->info.polygon.primitive_vertices == 2 ||
      dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE;
   return (vs->info.conservative && dyn->rs.conservative_mode ==
      VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT) ||
      dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias ||
      (lines && (dyn->rs.line.width != 1.0f ||
                 dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR));
}

/* Synthetic XFB passthrough stages are retained by the VS pipeline. */`)
    .replace('   return gs ? gs : vs && vs->info.has_geometry ? vs : NULL;',
      `   if (!gs && vs && vs->info.polygon_native && !kk_polygon_expansion_active(cmd))
      return NULL;
   return gs ? gs : vs && vs->info.has_geometry ? vs : NULL;`)
    .replace(`   /* Poly clips original triangles before applying bias. Native bias is
    * disabled while this path is active, including FILL/LINE representations. */
   bool controlled_bias = dyn->rs.depth_bias.enable &&
      (dyn->rs.depth_bias.exact || dyn->rs.depth_bias.representation !=
         VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT);
   bool conservative = vs->info.conservative && dyn->rs.conservative_mode ==
      VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
   if (conservative != gfx->conservative) {
      gfx->conservative = conservative;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
   }
   bool point = conservative || (vs->info.polygon_point &&
      (dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT || controlled_bias ||
       ((vs->info.polygon.primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE) &&
        (dyn->rs.line.width != 1.0f || dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR))));`,
      `   bool conservative = vs->info.conservative && dyn->rs.conservative_mode ==
      VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
   if (conservative != gfx->conservative) {
      gfx->conservative = conservative;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
   }
   bool point = kk_polygon_expansion_active(cmd);`));
}

export function smoothStipple({edit, edits}) {
  edits.set('src/kosmickrisp/libkk/kk_line_dashes.h',
    fs.readFileSync(new URL('./kosmickrisp-line-dashes.h', import.meta.url), 'utf8'));
  edits.set('src/kosmickrisp/vulkan/kk_lines_nir.h',
    fs.readFileSync(new URL('./kosmickrisp-lines-nir.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .rectangularLines = true,', `      .rectangularLines = true,
      .smoothLines = true,
      .stippledRectangularLines = false,
      .stippledBresenhamLines = true,
      .stippledSmoothLines = false,`)
    .replace('      .extendedDynamicState3LineRasterizationMode = true,',
      '      .extendedDynamicState3LineRasterizationMode = true,\n      .extendedDynamicState3LineStippleEnable = true,'));
  edit('src/poly/geometry_capture.h', s => s.replace('   float line_width;',
    `   float line_width;
   uint32_t line_stipple_enable;
   uint32_t line_stipple_factor;
   uint32_t line_stipple_pattern;
   uint32_t line_strip;
   uint64_t line_phases;
   uint64_t restart_strips;`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('         .line_mode = dyn->rs.line.mode,', `         .line_mode = dyn->rs.line.mode,
         .line_stipple_enable = dyn->rs.line.stipple.enable,
         .line_stipple_factor = dyn->rs.line.stipple.factor,
         .line_stipple_pattern = dyn->rs.line.stipple.pattern,
         .line_strip = !cmd->state.shaders[MESA_SHADER_VERTEX]->info.api_geometry &&
            !cmd->state.shaders[MESA_SHADER_TESS_EVAL] &&
            (dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
             dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY),`)
    .replace('                 dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR));',
      `                 dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR ||
                 dyn->rs.line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
                 dyn->rs.line.stipple.enable));`)
    .replace('      .restart_index = data->restart_index,\n      .index_buffer_size_el',
      `      .restart_index = data->restart_index,
      .capture = cmd->state.gfx.polygon_points &&
         cmd->vk.dynamic_graphics_state.rs.line.stipple.enable ? cmd->state.gfx.polygon_capture : 0,
      .index_buffer_size_el`));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('      state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR ||',
      `      state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR ||
      state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
      state->rs->line.stipple.enable ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE) ||`)
    .replace('static nir_shader *\nkk_polygon_vertex_shader(',
      '#include "kk_lines_nir.h"\n\nstatic nir_shader *\nkk_polygon_vertex_shader(')
    .replace('   unsigned packed = 0;\n   u_foreach_bit64(slot, info->outputs) {',
      `   nir_def *line_coord = nir_vec4(&b, nir_channel(&b, coord, 0),
      nir_channel(&b, coord, 1), nir_imm_float(&b, 0), nir_imm_float(&b, 0));
   unsigned packed = 0;
   u_foreach_bit64(slot, info->outputs) {`)
    .replace('            nir_def *rectangle_offset = nir_fmul(&b, perpendicular, nir_fmul(&b, side, line_width));',
      `            nir_def *smooth = line_effects ? nir_ieq_imm(&b, line_mode,
               VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH) : nir_imm_false(&b);
            nir_def *smooth_pad = nir_bcsel(&b, smooth, nir_imm_float(&b, 1), nir_imm_float(&b, 0));
            nir_def *expanded_width = nir_fadd(&b, line_width, smooth_pad);
            nir_def *endpoint_pad = nir_fmul_imm(&b, nir_fmul(&b, smooth_pad,
               nir_fadd_imm(&b, nir_fmul_imm(&b, nir_u2f32(&b, endpoint), 2), -1)), 0.5f);
            nir_def *length = nir_fsqrt(&b, length2);
            nir_def *tangent = nir_fmul(&b, delta,
               nir_frsq(&b, nir_fmax(&b, length2, nir_imm_float(&b, 1.0e-30f))));
            nir_def *rectangle_offset = nir_fadd(&b,
               nir_fmul(&b, perpendicular, nir_fmul(&b, side, expanded_width)),
               nir_fmul(&b, tangent, endpoint_pad));`)
    .replace('            nir_def *expanded = nir_fadd(&b, screen,',
      `            nir_def *expanded_coord = line_coord;
            if (line_effects) {
            nir_def *phase_address = nir_iadd(&b,
               kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
               nir_u2u64(&b, nir_iadd_imm(&b,
                  nir_imul_imm(&b, segment, 2 * (slots + 1) * 16), slots * 16 + 4)));
            nir_def *phase = nir_load_global_constant(&b, 1, 32, phase_address, .align_mul = 4);
            nir_def *metric = nir_bcsel(&b, bresenham,
               nir_fmax(&b, nir_fabs(&b, dx), nir_fabs(&b, dy)), length);
            nir_def *along = nir_fadd(&b, nir_fmul(&b, nir_u2f32(&b, endpoint), length), endpoint_pad);
            expanded_coord = nir_vec4(&b,
               nir_fadd(&b, phase, nir_fadd(&b,
                  nir_fmul(&b, nir_u2f32(&b, endpoint), metric), endpoint_pad)),
               nir_fmul(&b, side, expanded_width), along, length);
            }
            nir_def *expanded = nir_fadd(&b, screen,`)
    .replace('            screen = nir_if_phi(&b, expanded, plain_screen);',
      '            screen = nir_if_phi(&b, expanded, plain_screen);\n            if (line_effects) line_coord = nir_if_phi(&b, expanded_coord, line_coord);')
    .replace('   nir_store_output(&b, coord, nir_imm_int(&b, 0), .src_type = nir_type_float32,',
      `   if (line_effects)
      nir_store_output(&b, line_coord, nir_imm_int(&b, 0), .src_type = nir_type_float32,
         .io_semantics.location = VARYING_SLOT_FOGC, .io_semantics.num_slots = 1);
   nir_store_output(&b, coord, nir_imm_int(&b, 0), .src_type = nir_type_float32,`));
  edit('src/kosmickrisp/compiler/msl_iomap.c', s => s.replace(
    '   [VARYING_SLOT_PNTC] = {"polygon_point_coord", .user = true},',
    '   [VARYING_SLOT_PNTC] = {"polygon_point_coord", .user = true},\n   [VARYING_SLOT_FOGC] = {"polygon_line_coord", .user = true},'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('#include "kk_polygon_nir.h"\n', '')
    .replace('static void\nkk_lower_fs(', '#include "kk_polygon_nir.h"\n\nstatic void\nkk_lower_fs(')
    .replace('kk_polygon_vertex_shader(&shader->info.polygon, nir->options);',
      `kk_polygon_vertex_shader(&shader->info.polygon, nir->options,
            state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
            state->rs->line.stipple.enable ||
            BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE) ||
            BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE));`)
    .replace('   /* Must be lowered before blending */',
      `   if (state && state->rs &&
       (state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
        BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE)))
      nir_shader_intrinsics_pass(nir, kk_line_alpha, nir_metadata_control_flow, NULL);

   /* Must be lowered before blending */`)
    .replace('      shader->info.polygon_point = true;\n      if (kk_polygon_regular_possible(dev, state))\n         kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);',
      `      kk_line_fragment(point_fs, true, state);
      NIR_PASS(_, point_fs, kk_nir_lower_descriptors, robustness, 0, NULL);
      shader->info.polygon_point = true;
      if (kk_polygon_regular_possible(dev, state))
         kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);`)
    .replace('   unsigned num_cull_distances =',
      '   if (stage == MESA_SHADER_FRAGMENT) kk_line_fragment(nir, false, state);\n\n   unsigned num_cull_distances =')
    .replace('         kk_compile_msl(pdev, native_pair[1], state, &shaders[0]->polygon_native_fs);',
      '         kk_line_fragment(native_pair[1], false, state);\n         kk_compile_msl(pdev, native_pair[1], state, &shaders[0]->polygon_native_fs);'));
  edit('src/poly/nir/poly_nir_capture_gs.h', s => s.replace(
    'nir_store_global(b, nir_vec4(b, a, c, c, nir_imm_int(b, 0)), address);',
    'nir_store_global(b, nir_vec4(b, a, c, c, intr->src[3].ssa), address);'));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('   p->invocations = geometry->input_primitives;',
      `   p->invocations = geometry->input_primitives;
   p->line_phases = p->line_stipple_enable && p->primitive_vertices == 2 ?
      (uintptr_t)poly_heap_alloc(heap, triangles * 8, 8) : 0;`)
    .replace('   clipped[id] = 0;',
      '   clipped[id] = 0;\n   if (p->line_phases) ((global uint64_t *)p->line_phases)[id] = 0;')
    .replace('   counts[id] = p->primitive_vertices == 2 ? count :',
      `   if (p->line_phases) {
      float2 a = (float2)(output[0].position[0], output[0].position[1]) / output[0].position[3];
      float2 b = (float2)(output[1].position[0], output[1].position[1]) / output[1].position[3];
      float2 d = (b - a) * (float2)(p->viewport[viewport][2], p->viewport[viewport][3]) * 0.5f;
      float length = p->line_mode == 2 ? fmax(fabs(d.x), fabs(d.y)) : sqrt(d.x * d.x + d.y * d.y);
      ((global uint64_t *)p->line_phases)[id] = (uint64_t)ceil(length);
   }
   counts[id] = p->primitive_vertices == 2 ? count :`)
    .replace('                    1, 0, 1024);',
      `                    1, 0, 1024);
   if (p->line_phases) {
      global uint64_t *phase = (global uint64_t *)p->line_phases;
      uint tid = cl_local_id.x;
      uint64_t carry = 0;
      for (uint base = 0; base < p->clip_grid[0]; base += 1024) {
         uint64_t value = base + tid < p->clip_grid[0] ? phase[base + tid] : 0;
         /* Reuse the original 32-subgroup scratch. A low-word prefix wraps
          * exactly when its result is less than this element's low word. */
         uint2 low = poly_work_group_scan_inclusive_add((uint)value, scratch);
         uint2 high = poly_work_group_scan_inclusive_add(
            (uint)(value >> 32) + (uint)(low.x < (uint)value), scratch);
         uint64_t prefix = ((uint64_t)high.x << 32) | low.x;
         uint64_t total = ((uint64_t)high.y << 32) | low.y;
         if (base + tid < p->clip_grid[0]) phase[base + tid] = carry + prefix;
         carry += total;
      }
   }`)
    .replace('   for (uint i = 0; i < output_count; ++i) {',
      `   uint64_t phase = 0;
   if (p->line_phases) {
      uint maximum = sub_sat(p->vertices_out, 1u);
      uint first_primitive = (invocation * p->streams + stream) * maximum + vertices.w;
      if (p->line_strip) {
         uint per_instance = max(geometry->grid[0], 1u);
         uint instance_first = invocation - invocation % per_instance;
         uint strip_first = p->restart_strips ? ((global uint *)p->restart_strips)[invocation % per_instance] : 0;
         first_primitive = (instance_first + strip_first) * p->streams * maximum;
      }
      global uint64_t *phases = (global uint64_t *)p->line_phases;
      phase = (id ? phases[id - 1] : 0) - (first_primitive ? phases[first_primitive - 1] : 0);
      phase %= max(p->line_stipple_factor, 1u) * 16u;
   }
   float polygon_phase = 0;
   for (uint i = 0; i < output_count; ++i) {`)
    .replace('      output[packed] = (uint4)(front, 0, 0, 0);',
      `      if (p->line_stipple_enable && p->line_expand && p->primitive_vertices == 3 && (i & 1u)) {
         struct kk_polygon_vertex a = polygon[(i / 2u) % count];
         struct kk_polygon_vertex b = polygon[(i / 2u + 1u) % count];
         float2 d = ((float2)(b.position[0], b.position[1]) / b.position[3] -
                     (float2)(a.position[0], a.position[1]) / a.position[3]) *
                    (float2)(p->viewport[viewport][2], p->viewport[viewport][3]) * 0.5f;
         polygon_phase += p->line_mode == 2 ? ceil(fmax(fabs(d.x), fabs(d.y))) : ceil(sqrt(d.x * d.x + d.y * d.y));
      }
      output[packed] = (uint4)(front, as_uint((float)phase + polygon_phase), 0, 0);`));
  // Carry restart boundaries through the existing Poly unroller. The original
  // entry point remains a wrapper with no strip metadata for its other users.
  edit('src/poly/cl/restart.h', s => s
    .replace('poly_unroll_geometry(global uint32_t *out_draw,',
      'poly_unroll_geometry_strips(global uint32_t *out_draw,')
    .replace('                     local void *scratch)\n{\n   uint tid = cl_local_id.x;\n   uint count = in_draw[0];',
      '                     local void *scratch, global uint32_t *strip_starts)\n{\n   uint tid = cl_local_id.x;\n   uint count = in_draw[0];')
    .replace('      for (uint i = tid; i < subprims; i += cl_local_size.x) {',
      '      for (uint i = tid; i < subprims; i += cl_local_size.x) {\n         if (strip_starts) strip_starts[out_prims_base + i] = out_prims_base;')
    .replace('static inline void\npoly_unroll_restart(',
      `static inline void
poly_unroll_geometry(global uint32_t *out_draw, global struct poly_heap *heap,
   constant uint *in_draw, uint64_t index_buffer, uint32_t index_buffer_range_el,
   uint32_t in_index_size_B, uint32_t out_index_size_B, uint32_t restart_index,
   uint32_t flatshade_first, uint32_t emulate_flatshade_last, enum mesa_prim mode,
   local void *scratch)
{
   poly_unroll_geometry_strips(out_draw, heap, in_draw, index_buffer,
      index_buffer_range_el, in_index_size_B, out_index_size_B, restart_index,
      flatshade_first, emulate_flatshade_last, mode, scratch, NULL);
}

static inline void
poly_unroll_restart(`));
  edit('src/kosmickrisp/libkk/kk_draws.cl', s => s
    .replace('#include "poly/cl/restart.h"',
      '#include "poly/cl/restart.h"\n#include "poly/geometry_capture.h"')
    .replace('                     uint32_t mode)\n{\n   POLY_DECL_UNROLL_RESTART_SCRATCH',
      '                     uint32_t mode, uint64_t capture)\n{\n   POLY_DECL_UNROLL_RESTART_SCRATCH')
    .replace(`   poly_unroll_restart(out_draw, heap, in_draw, index_buffer,
                       index_buffer_size_el, index_size_B, restart_index,
                       flatshade_first, mode, scratch);`,
      `   global uint *strip_starts = NULL;
   if (capture) {
      global struct poly_geometry_capture *p = (global struct poly_geometry_capture *)capture;
      if (cl_local_id.x == 0)
         p->restart_strips = (uintptr_t)poly_heap_alloc(heap,
            u_decomposed_prims_for_vertices(mode, in_draw[0]) * 4, 4);
      barrier(CLK_GLOBAL_MEM_FENCE);
      strip_starts = (global uint *)p->restart_strips;
   }
   poly_unroll_geometry_strips(out_draw, heap, in_draw, index_buffer,
      index_buffer_size_el, index_size_B, index_size_B, restart_index,
      flatshade_first, false, mode, scratch, strip_starts);`));
}

export function pureEarlyStipple({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '   /* Must be lowered before blending */',
    '   kk_line_lower_pure_early(nir, state);\n\n   /* Must be lowered before blending */'));
}

export function sideEffectEarlyStipple({edit}) {
  edit('src/poly/geometry_capture.h', s => s
    .replace('/* Per-draw storage for polygon rasterization after geometry assembly. */',
      `enum kk_line_stipple_flags {
   KK_LINE_STIPPLE_ENABLED = 1u,
   KK_LINE_STIPPLE_GEOMETRY = 2u,
   KK_LINE_STIPPLE_PHASE_READY = 4u,
};

/* Per-draw storage for polygon rasterization after geometry assembly. */`)
    .replace('   uint32_t line_stipple_enable;', '   uint32_t line_stipple_flags;'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s.replace(
    '         bool uses_flat_varyings;', '         bool uses_flat_varyings;\n         bool line_geometry_stipple;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '   gather_shader_info(shader, nir, state);',
    `   gather_shader_info(shader, nir, state);
   if (stage == MESA_SHADER_FRAGMENT)
      shader->info.fs.line_geometry_stipple = kk_line_geometry_stipple(nir, state);`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    '         .line_stipple_enable = dyn->rs.line.stipple.enable,',
    `         .line_stipple_flags = dyn->rs.line.stipple.enable ?
            KK_LINE_STIPPLE_ENABLED |
            ((fs && fs->info.fs.line_geometry_stipple &&
              (capture->primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE)) ?
               KK_LINE_STIPPLE_GEOMETRY : 0) : 0,`));
  edit('src/kosmickrisp/vulkan/kk_lines_nir.h', s => s
    .replace('offsetof(struct poly_geometry_capture, line_stipple_enable)',
      'offsetof(struct poly_geometry_capture, line_stipple_flags)'));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s.replace(
    '   libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);',
    `   libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   if (fs && fs->info.fs.line_geometry_stipple && dyn->rs.line.stipple.enable &&
       (vs->info.polygon.primitive_vertices == 2 || dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE)) {
      libkk_polygon_dash_counts(cmd, kk_grid_indirect(capture +
         offsetof(struct poly_geometry_capture, clip_grid)), true, geometry_params, kk_heap(cmd));
      libkk_polygon_prefix_sum(cmd, kk_grid_1d(1024), true, geometry_params);
   }`));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('#include "kk_polygon.h"', '#include "kk_polygon.h"\n#include "kk_line_dashes.h"')
    .replaceAll('p->line_stipple_enable', 'p->line_stipple_flags')
    .replace('   global struct kk_polygon_vertex *saved = poly_heap_alloc(\n      heap, count * sizeof(struct kk_polygon_vertex), 4);',
      `   uint saved_bytes = count * sizeof(struct kk_polygon_vertex);
   bool geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   if (geometry_stipple) saved_bytes = align(saved_bytes, 8) + 8;
   global struct kk_polygon_vertex *saved = poly_heap_alloc(
      heap, saved_bytes, geometry_stipple ? 8 : 4);
   if (geometry_stipple) {
      global uint4 *primitives = (global uint4 *)p->triangles;
      /* GS vertices_out is 16-bit; the upper word is free strip metadata. */
      uint4 primitive = primitives[id];
      primitive.w = (primitive.w & 0xffffu) | (count << 16);
      primitives[id] = primitive;
   }`)
    .replace('      ((global uint64_t *)p->line_phases)[id] = (uint64_t)ceil(length);',
      `      ((global uint64_t *)p->line_phases)[id] =
         (p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY) && p->line_mode == 2 ?
            kk_line_steps(kk_line_screen(p, viewport, &output[0]),
                          kk_line_screen(p, viewport, &output[1]), p->line_mode, p->line_width) :
            (uint64_t)ceil(length);`)
    .replace('   if (p->line_phases) {\n      global uint64_t *phase =',
      '   if (p->line_phases && !(p->line_stipple_flags & KK_LINE_STIPPLE_PHASE_READY)) {\n      global uint64_t *phase =')
    .replace('         carry += total;\n      }\n   }',
      '         carry += total;\n      }\n      if (cl_local_id.x == 0) p->line_stipple_flags |= KK_LINE_STIPPLE_PHASE_READY;\n   }')
    .replace('maximum + vertices.w;', 'maximum + (vertices.w & 0xffffu);')
    .replace('      if (p->line_stipple_flags && p->line_expand && p->primitive_vertices == 3 && (i & 1u)) {',
      '      if (!geometry_stipple && p->line_stipple_flags && p->line_expand && p->primitive_vertices == 3 && (i & 1u)) {')
    .replace('   bool front = clipped & 1;\n   global struct kk_polygon_vertex *polygon =',
      `   bool front = clipped & 1;
   bool geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   if (geometry_stipple) count = vertices.w >> 16;
   global struct kk_polygon_vertex *polygon =`)
    .replace('      struct kk_polygon_vertex vertex = polygon[vertex_index];',
      `      struct kk_polygon_vertex vertex;
      float original_along = 0, original_length = 0;
      if (geometry_stipple) {
         global struct kk_line_dash *spans =
            (global struct kk_line_dash *)*kk_line_dash_pointer(polygon, count);
         struct kk_line_dash span = spans[i / 2u];
         struct kk_polygon_vertex a = polygon[span.edge];
         struct kk_polygon_vertex b = polygon[(span.edge + 1) % count];
         float t = (i & 1u) ? span.last : span.first;
         float2 screen_a = kk_line_screen(p, viewport, &a);
         float2 screen_b = kk_line_screen(p, viewport, &b);
         float2 delta = screen_b - screen_a;
         original_length = sqrt(delta.x * delta.x + delta.y * delta.y);
         original_along = t * original_length;
         /* Screen-linear cuts require homogeneous interpolation of clip
          * coordinates and the original triangle's interpolation weights. */
         float homogeneous = t * a.position[3] /
            ((1.0f - t) * b.position[3] + t * a.position[3]);
         vertex = kk_polygon_intersection(&a, &b, homogeneous);
      } else {
         vertex = polygon[vertex_index];
      }`)
    .replace('      output[packed] = (uint4)(front, as_uint((float)phase + polygon_phase), 0, 0);',
      `      output[packed] = (uint4)(front, as_uint((float)phase + polygon_phase),
         as_uint(original_along), as_uint(original_length));`)
    .replace('KERNEL(1024)\nlibkk_polygon_prefix_sum(',
      fs.readFileSync(new URL('./kosmickrisp-line-dashes.cl', import.meta.url), 'utf8') +
      '\nKERNEL(1024)\nlibkk_polygon_prefix_sum('));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('   nir_def *line_width = kk_polygon_load(&b, 1, 32,',
      `   nir_def *stipple_flags = kk_polygon_load(&b, 1, 32,
      offsetof(struct poly_geometry_capture, line_stipple_flags), 4);
   nir_def *geometry_stipple = nir_ine_imm(&b,
      nir_iand_imm(&b, stipple_flags, KK_LINE_STIPPLE_GEOMETRY), 0);
   nir_def *line_width = kk_polygon_load(&b, 1, 32,`)
    .replace('            nir_def *endpoint_pad = nir_fmul_imm(&b, nir_fmul(&b, smooth_pad,',
      `            nir_def *packet = nir_load_global_constant(&b, 4, 32,
               nir_iadd_imm(&b, address, slots * 16), .align_mul = 16);
            nir_def *original_along = nir_channel(&b, packet, 2);
            nir_def *original_length = nir_channel(&b, packet, 3);
            nir_def *original_endpoint = nir_ior(&b,
               nir_feq_imm(&b, original_along, 0), nir_feq(&b, original_along, original_length));
            nir_def *pad_enabled = nir_bcsel(&b,
               nir_iand(&b, geometry_stipple, nir_inot(&b, original_endpoint)),
               nir_imm_float(&b, 0), smooth_pad);
            nir_def *endpoint_pad = nir_fmul_imm(&b, nir_fmul(&b, pad_enabled,`)
    .replace('               nir_fmul(&b, side, expanded_width), along, length);',
      `               nir_fmul(&b, side, expanded_width),
               nir_bcsel(&b, geometry_stipple, nir_fadd(&b, original_along, endpoint_pad), along),
               nir_bcsel(&b, geometry_stipple, original_length, length));`));
}

export function sampleBoundaryStipple({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('      params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;',
      `      if ((point.line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY) && point.line_mode != 2) {
         const struct vk_sample_locations_state *locations = dyn->ms.sample_locations_enable ?
            dyn->ms.sample_locations : vk_standard_sample_locations_state(MAX2(dyn->ms.rasterization_samples, 1u));
         uint32_t samples = locations->per_pixel;
         uint32_t header[2] = {samples, 0};
         struct kk_ptr capture_data = kk_pool_alloc(cmd,
            sizeof(point) + sizeof(header) + samples * sizeof(VkSampleLocationEXT), 8);
         memcpy(capture_data.cpu, &point, sizeof(point));
         memcpy((char *)capture_data.cpu + sizeof(point), header, sizeof(header));
         VkSampleLocationEXT *output_locations = (void *)((char *)capture_data.cpu +
            sizeof(point) + sizeof(header));
         for (uint32_t i = 0; i < samples; ++i) {
            output_locations[i].x = CLAMP(locations->locations[i].x,
               KK_MIN_SAMPLE_LOCATION, KK_MAX_SAMPLE_LOCATION);
            output_locations[i].y = CLAMP(locations->locations[i].y,
               KK_MIN_SAMPLE_LOCATION, KK_MAX_SAMPLE_LOCATION);
         }
         params.capture = gfx->polygon_capture = capture_data.gpu;
      } else {
         params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;
      }`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    '#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "vk_standard_sample_locations.h"'));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('float length = p->line_mode == 2 ? fmax(fabs(d.x), fabs(d.y)) : sqrt(d.x * d.x + d.y * d.y);',
      'float length = p->line_mode == 2 ? fmax(fabs(d.x), fabs(d.y)) : kk_line_length(d);')
    .replace('         original_length = sqrt(delta.x * delta.x + delta.y * delta.y);',
      '         original_length = kk_line_length(delta);')
    .replace('(p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY) && p->line_mode == 2 ?',
      'p->line_mode == 2 ?')
    .replace('   counts[id] = written * 2u;',
      '   counts[id] = written * (p->line_mode == 2 ? 2u : 4u);')
    .replace('   p->output = (uintptr_t)poly_heap_alloc(heap, count * (p->vertex_stride + 16), 16);',
      `   bool geometry_stipple = p->line_stipple_flags & KK_LINE_STIPPLE_GEOMETRY;
   uint packet_stride = p->vertex_stride + 16 + (geometry_stipple ? 16 : 0);
   p->output = (uintptr_t)poly_heap_alloc(heap, count * packet_stride, 16);`)
    .replace('   p->draw[0] = p->line_expand ? count / 2u * line_vertices :',
      `   if (geometry_stipple && p->line_mode == 2) line_vertices = 2;
   p->draw[0] = geometry_stipple && p->line_mode != 2 ? count / 4u * 6u :
      p->line_expand ? count / 2u * line_vertices :`)
    .replace('   float polygon_phase = 0;',
      `   float bres_origin = 0;
   if (p->line_stipple_flags && p->primitive_vertices == 2 && p->line_expand && p->line_mode == 2) {
      float2 a = kk_line_screen(p, viewport, &polygon[0]);
      float2 b = kk_line_screen(p, viewport, &polygon[1]);
      int4 bounds = kk_line_wide_bres_bounds(a, b, p->line_width);
      bres_origin = bounds.z * ((float)bounds.x - floor(bounds.w ? a.x : a.y));
   }
   uint packet_stride = p->vertex_stride + 16 + (geometry_stipple ? 16 : 0);
   float polygon_phase = 0;`)
    .replace('      float original_along = 0, original_length = 0;',
      '      float original_along = 0, original_length = 0;\n      float4 original_endpoints = (float4)(0);')
    .replace('         struct kk_line_dash span = spans[i / 2u];',
      '         struct kk_line_dash span = spans[i / (p->line_mode == 2 ? 2u : 4u)];')
    .replace('         float t = (i & 1u) ? span.last : span.first;',
      '         float t = (i & 1u) ? span.last : span.first;')
    .replace('         original_along = t * original_length;',
      `         original_along = p->line_mode == 2 ? span.rectangle.x : 0;
         original_endpoints = (float4)(screen_a, screen_b);
         float2 rectangle_point = (float2)(0);
         if (p->line_mode != 2) {
            uint corner = i & 3u;
            rectangle_point = (float2)((corner & 2u) ? span.rectangle.z : span.rectangle.x,
                                        (corner & 1u) ? span.rectangle.w : span.rectangle.y);
            float along = ((rectangle_point.x - screen_a.x) * delta.x +
                           (rectangle_point.y - screen_a.y) * delta.y) / original_length;
            float cap = p->line_mode == 3 ? 0.5f : 0;
            t = (along + cap) / (original_length + 2 * cap);
         }
         phase = (uint64_t)span.phase;`)
    .replace('         vertex = kk_polygon_intersection(&a, &b, homogeneous);',
      `         vertex = kk_polygon_intersection(&a, &b, homogeneous);
         if (p->line_mode != 2) {
            vertex.position[0] = ((rectangle_point.x - p->viewport[viewport][0]) *
               (2.0f / p->viewport[viewport][2]) - 1.0f) * vertex.position[3];
            vertex.position[1] = ((rectangle_point.y - p->viewport[viewport][1]) *
               (2.0f / p->viewport[viewport][3]) - 1.0f) * vertex.position[3];
         }`)
    .replace('         (uint64_t)(first + i) * (p->vertex_stride + 16));',
      '         (uint64_t)(first + i) * packet_stride);')
    .replace('      output[packed] = (uint4)(front, as_uint((float)phase + polygon_phase),',
      `      if (!geometry_stipple && p->line_stipple_flags && p->primitive_vertices == 3 &&
          p->line_expand && p->line_mode == 2) {
         float2 a = kk_line_screen(p, viewport, &polygon[(i / 2u) % count]);
         float2 b = kk_line_screen(p, viewport, &polygon[(i / 2u + 1u) % count]);
         int4 bounds = kk_line_wide_bres_bounds(a, b, p->line_width);
         bres_origin = bounds.z * ((float)bounds.x - floor(bounds.w ? a.x : a.y));
      }
      output[packed] = (uint4)(front, as_uint((float)phase + polygon_phase - bres_origin),`)
    .replace('         as_uint(original_along), as_uint(original_length));',
      '         as_uint(original_along), as_uint(original_length));\n      if (geometry_stipple) output[packed + 1] = as_uint4(original_endpoints);'));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('   nir_def *line_width = kk_polygon_load(&b, 1, 32,',
      `   nir_def *rectangle_stipple = nir_iand(&b, geometry_stipple, nir_inot(&b, bresenham));
   nir_def *packet_stride = nir_imul_imm(&b,
      nir_iadd_imm(&b, nir_b2i32(&b, geometry_stipple), util_bitcount64(info->outputs) + 1), 16);
   nir_def *line_width = kk_polygon_load(&b, 1, 32,`)
    .replace('      nir_imul_imm(&b, copies, 2), nir_imm_int(&b, 6));',
      '      nir_bcsel(&b, geometry_stipple, nir_imm_int(&b, 2), nir_imul_imm(&b, copies, 2)), nir_imm_int(&b, 6));')
    .replace('         nir_imul_imm(&b, segment, 2 * (util_bitcount64(info->outputs) + 1) * 16),',
      '         nir_imul(&b, segment, nir_imul(&b, packet_stride, nir_bcsel(&b, rectangle_stipple, nir_imm_int(&b, 4), nir_imm_int(&b, 2)))),')
    .replace('      nir_iadd(&b, nir_imul_imm(&b, segment, 2), endpoint),',
      `      nir_bcsel(&b, rectangle_stipple,
         nir_iadd(&b, nir_imul_imm(&b, segment, 4), line_corner),
         nir_iadd(&b, nir_imul_imm(&b, segment, 2), endpoint)),`)
    .replace('      nir_u2u64(&b, nir_imul_imm(&b, point, (slots + 1) * 16)));',
      '      nir_u2u64(&b, nir_imul(&b, point, packet_stride)));')
    .replace(`   nir_def *line_coord = nir_vec4(&b, nir_channel(&b, coord, 0),
      nir_channel(&b, coord, 1), nir_imm_float(&b, 0), nir_imm_float(&b, 0));`,
      `   nir_def *line_coord = nir_imm_vec4(&b, 0, 0, 0, 0);
   nir_def *line_phase = nir_imm_float(&b, 0);
   if (line_effects) {
      nir_push_if(&b, is_line);
      nir_def *metadata = nir_load_global_constant(&b, 4, 32,
         nir_iadd_imm(&b, address, slots * 16), .align_mul = 16);
      nir_def *prefix = nir_channel(&b, metadata, 1);
      nir_push_if(&b, geometry_stipple);
      nir_def *saved_endpoints = nir_load_global_constant(&b, 4, 32,
         nir_iadd_imm(&b, address, (slots + 1) * 16), .align_mul = 16);
      nir_push_else(&b, NULL);
      nir_def *base = nir_iadd(&b,
         kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
         nir_u2u64(&b, nir_imul(&b, nir_imul_imm(&b, segment, 2), packet_stride)));
      unsigned position_slot = util_bitcount64(info->outputs & BITFIELD64_MASK(VARYING_SLOT_POS)) * 16;
      nir_def *a = nir_load_global_constant(&b, 4, 32, nir_iadd_imm(&b, base, position_slot), .align_mul = 16);
      nir_def *bpos = nir_load_global_constant(&b, 4, 32,
         nir_iadd_imm(&b, nir_iadd(&b, base, nir_u2u64(&b, packet_stride)), position_slot), .align_mul = 16);
      nir_def *screen_a = nir_ffma(&b,
         nir_fmul_imm(&b, nir_fadd_imm(&b, nir_fdiv(&b, nir_channels(&b, a, 3), nir_channel(&b, a, 3)), 1), 0.5f),
         nir_channels(&b, vp, 12), nir_channels(&b, vp, 3));
      nir_def *screen_b = nir_ffma(&b,
         nir_fmul_imm(&b, nir_fadd_imm(&b, nir_fdiv(&b, nir_channels(&b, bpos, 3), nir_channel(&b, bpos, 3)), 1), 0.5f),
         nir_channels(&b, vp, 12), nir_channels(&b, vp, 3));
      nir_def *native_endpoints = nir_vec4(&b, nir_channel(&b, screen_a, 0), nir_channel(&b, screen_a, 1),
         nir_channel(&b, screen_b, 0), nir_channel(&b, screen_b, 1));
      nir_pop_if(&b, NULL);
      nir_def *endpoints = nir_if_phi(&b, saved_endpoints, native_endpoints);
      nir_pop_if(&b, NULL);
      line_coord = nir_if_phi(&b, endpoints, line_coord);
      line_phase = nir_if_phi(&b, prefix, line_phase);
   }`)
    .replace('         nir_push_if(&b, is_line);\n         {',
      '         nir_push_if(&b, nir_iand(&b, is_line, nir_inot(&b, rectangle_stipple)));\n         {')
    .replace('                  nir_imul_imm(&b, other_index, (slots + 1) * 16),',
      '                  nir_imul(&b, other_index, packet_stride),')
    .replace(`            nir_def *replica_offset = nir_fsub(&b, nir_u2f32(&b, copy),
               nir_fmul_imm(&b, nir_u2f32(&b, nir_iadd_imm(&b, copies, -1)), 0.5f));`,
      `            nir_def *replica_offset = nir_bcsel(&b, geometry_stipple,
               nir_channel(&b, packet, 2),
               nir_fsub(&b, nir_u2f32(&b, copy),
                  nir_fmul_imm(&b, nir_u2f32(&b, nir_iadd_imm(&b, copies, -1)), 0.5f)));`)
    .replace(/            nir_def \*expanded_coord = line_coord;\n            if \(line_effects\) \{[\s\S]*?            \}\n            nir_def \*expanded =/,
      '            nir_def *expanded =')
    .replace('            if (line_effects) line_coord = nir_if_phi(&b, expanded_coord, line_coord);', '')
    .replace('   nir_store_output(&b, coord, nir_imm_int(&b, 0), .src_type = nir_type_float32,',
      `   nir_def *point_payload = coord;
   if (line_effects) point_payload = nir_vec4(&b,
      nir_bcsel(&b, is_line, line_phase, nir_channel(&b, coord, 0)),
      nir_channel(&b, coord, 1), nir_u2f32(&b, front), nir_imm_float(&b, 0));
   nir_store_output(&b, point_payload, nir_imm_int(&b, 0), .src_type = nir_type_float32,`));
  /* Keep PointCoord's payload width paired with the selected raster VS. */
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s
    .replace('   nir_def *coord = nir_load_interpolated_input(b, 2, 32, barycentric,',
      '   nir_def *coord = nir_load_interpolated_input(b, data ? 4 : 2, 32, barycentric,')
    .replace('   nir_def_replace(&intr->def, coord);',
      '   nir_def_replace(&intr->def, nir_trim_vector(b, coord, 2));'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,\n                                 nir_metadata_control_flow, NULL);',
    `      bool point_line_effects = state->rs->line.mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH ||
         state->rs->line.stipple.enable ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_MODE) ||
         BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_RS_LINE_STIPPLE_ENABLE);
      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,
                                 nir_metadata_control_flow, point_line_effects ? (void *)1 : NULL);`));
}
