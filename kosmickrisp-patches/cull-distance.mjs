// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const sourceFiles = ['kosmickrisp-patches/cull-distance-indirect.h'];
// CullDistance is evaluated on assembled primitives before rasterization.
export const id = 'cull-distance';
export function apply({edit, edits}) {
  edit('src/compiler/nir/nir_shader_compiler_options.h', s => s.replace(
    'typedef struct nir_shader_compiler_options {',
    'typedef struct nir_shader_compiler_options {\n   /* Keep clip/cull IO in built-in slots for primitive assembly. */\n   bool preserve_clip_cull_io;'));
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => s.replace(
    'static const nir_shader_compiler_options kk_nir_options = {',
    'static const nir_shader_compiler_options kk_nir_options = {\n   .preserve_clip_cull_io = true,'));
  edit('src/compiler/nir/nir_opt_varyings.c', s => s
    .replace('can_remove_varying(struct linkage_info *linkage, gl_varying_slot location)\n{',
      'can_remove_varying(struct linkage_info *linkage, gl_varying_slot location)\n{\n   if (linkage->producer_builder.shader->options->preserve_clip_cull_io &&\n       (location == VARYING_SLOT_CLIP_DIST0 || location == VARYING_SLOT_CLIP_DIST1 ||\n        location == VARYING_SLOT_CULL_DIST0 || location == VARYING_SLOT_CULL_DIST1))\n      return false;')
    .replace('   if (consumer->info.stage <= MESA_SHADER_GEOMETRY) {',
      '   if (consumer->info.stage <= MESA_SHADER_GEOMETRY &&\n       !producer->options->preserve_clip_cull_io) {'));

  // IO is already separated at translation, with each stage input's own
  // array layout. Do not reinterpret it using the output array's size.
  edit('src/kosmickrisp/compiler/msl_nir_lower_common.c', s => s.replace(
    '   NIR_PASS(_, nir, nir_separate_merged_clip_cull_io);\n', ''));

  edits.set('src/kosmickrisp/compiler/kk_cull_distance_indirect.h',
    fs.readFileSync(new URL('./cull-distance-indirect.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('void\nmsl_preprocess_nir(struct nir_shader *nir)',
      '#include "kk_cull_distance_indirect.h"\n\nvoid\nmsl_preprocess_nir(struct nir_shader *nir)')
    .replace('NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,',
      'NIR_PASS(_, nir, kk_cull_nir_lower_indirect_derefs_to_if_else_trees,'));

  edit('src/kosmickrisp/compiler/kk_mesh_iomap.h', s => s.replaceAll(
    '[[cull_primitive]]', '[[primitive_culled]]'));

  // Vulkan's common translation normally merges cull values into clip slots.
  // Keep each built-in's identity through variable and intrinsic lowering,
  // including stage inputs whose array sizes differ from their outputs.
  edit('src/compiler/spirv/nir_spirv.h', s => s.replace(
    'struct spirv_to_nir_options {',
    'struct spirv_to_nir_options {\n   bool separate_clip_cull_distance_arrays;'));
  edit('src/vulkan/runtime/vk_nir.c', s => s.replace(
    '   NIR_PASS(_, nir, nir_merge_clip_cull_distance_vars);',
    '   if (!spirv_options->separate_clip_cull_distance_arrays)\n      NIR_PASS(_, nir, nir_merge_clip_cull_distance_vars);'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '      .environment = NIR_SPIRV_VULKAN,',
    '      .environment = NIR_SPIRV_VULKAN,\n      .separate_clip_cull_distance_arrays = true,'));
  edit('src/poly/nir/poly_nir_passthrough_gs.c', s => s.replace(
    'key->cull_distance_array_size + key->clip_distance_array_size;',
    'adjusted_loc == VARYING_SLOT_CULL_DIST0 ?\n               key->cull_distance_array_size : key->clip_distance_array_size;'));

  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s.replace(
    '(kk_wide_line_possible(state) || state->rs->polygon_mode',
    '((!(state->shader_stages & VK_SHADER_STAGE_MESH_BIT_EXT) && dev->vk.enabled_features.shaderCullDistance) || kk_wide_line_possible(state) || state->rs->polygon_mode'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s
    .replace('      (kk_wide_line_possible(state) || poly_layer_replay);', '      (kk_wide_line_possible(state) || poly_layer_replay || dev->vk.enabled_features.shaderCullDistance);')
    .replace('         kk_compile_msl(pdev, native_pair[0], state, &shaders[0]->polygon_native);', '         NIR_PASS(_, native_pair[0], kk_nir_lower_polygon_scalar_outputs);\n         kk_compile_msl(pdev, native_pair[0], state, &shaders[0]->polygon_native);')
    .replace('   if (stage != MESA_SHADER_GEOMETRY && stage != MESA_SHADER_MESH)\n      msl_nir_lower_clip_cull_distance', '   if (stage == MESA_SHADER_FRAGMENT ||\n       (stage == MESA_SHADER_VERTEX && nir->info.next_stage == MESA_SHADER_FRAGMENT) ||\n       (stage == MESA_SHADER_TESS_EVAL && nir->info.next_stage == MESA_SHADER_FRAGMENT))\n      msl_nir_lower_clip_cull_distance')
    .replace('if (native_passthrough && native_vs) {',
      'if (native_passthrough && native_vs && !native_vs->info.cull_distance_array_size) {')
    .replace('      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,', '      if (!prev_stage || prev_stage->info.polygon.primitive_vertices != 1)\n      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,')
    .replace('u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_LINES);',
      'u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_LINES ||\n          u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_POINTS);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s
    .replace('return (vs->info.conservative && dyn->rs.conservative_mode ==',
      'return vs->info.polygon.cull_count || (vs->info.conservative && dyn->rs.conservative_mode ==')
    .replace('.polygon_mode = dyn->rs.polygon_mode,',
      '.polygon_mode = capture->primitive_vertices == 1 ? VK_POLYGON_MODE_FILL : dyn->rs.polygon_mode,')
    .replace('.cull_mode = capture->primitive_vertices == 2 ?', '.cull_mode = capture->primitive_vertices <= 2 ?'));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s
    .replace('vs->info.polygon.primitive_vertices != 2', 'vs->info.polygon.primitive_vertices > 2')
    .replace('      .primitive_type =\n',
      '      .primitive_type = vs->info.polygon.primitive_vertices == 1 ? MTL_PRIMITIVE_TYPE_POINT :\n'));
  edit('src/poly/nir/poly_nir_capture_gs.h', s => s
    .replaceAll('u_reduced_prim(b->shader->info.gs.output_primitive) == MESA_PRIM_LINES ? 2 : 3',
      'mesa_vertices_per_prim(u_reduced_prim(b->shader->info.gs.output_primitive))')
    .replaceAll('u_reduced_prim(gs->info.gs.output_primitive) == MESA_PRIM_LINES ? 2 : 3',
      'mesa_vertices_per_prim(u_reduced_prim(gs->info.gs.output_primitive))')
    .replace('         if (primitive_vertices == 2) {', '         if (primitive_vertices <= 2) {')
    .replace('nir_iadd_imm(b, CAPTURE_PARAM(b, vertices_out), -1)',
      'nir_iadd_imm(b, CAPTURE_PARAM(b, vertices_out), 1 - primitive_vertices)')
    .replace('nir_def *c = nir_iadd_imm(b, a, 1);',
      'nir_def *c = nir_iadd_imm(b, a, primitive_vertices - 1);'));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('uint count = p->primitive_vertices == 2 ?',
      'uint count = p->primitive_vertices == 1 ?\n      kk_polygon_clip_point(positions, clip, cull, p->clip_count, p->cull_count,\n                            p->depth_clip, p->negative_one_to_one, output) :\n      p->primitive_vertices == 2 ?')
    .replace('if (count < (p->primitive_vertices == 2 ? 2u : 3u))',
      'if (count < p->primitive_vertices)')
    .replace('bool front = p->primitive_vertices == 2 ||', 'bool front = p->primitive_vertices <= 2 ||')
    .replace('if (p->primitive_vertices != 2 && !defer_facing', 'if (p->primitive_vertices > 2 && !defer_facing')
    .replace('counts[id] = p->primitive_vertices == 2 ? count :', 'counts[id] = p->primitive_vertices <= 2 ? count :')
    .replace('uint count = p->primitive_vertices == 2 ? output_count :', 'uint count = p->primitive_vertices <= 2 ? output_count :')
    .replace('uint vertex_index = p->primitive_vertices == 2 ? i :', 'uint vertex_index = p->primitive_vertices <= 2 ? i :'));
  edit('src/kosmickrisp/libkk/kk_polygon.h', s => s.replace(/#endif\s*$/, `\nstatic inline unsigned
kk_polygon_clip_point(const float position[3][4],
                      const float clip[3][KK_POLYGON_CLIP_DISTANCES],
                      const float cull[3][KK_POLYGON_CLIP_DISTANCES],
                      unsigned clip_count, unsigned cull_count,
                      bool depth_clip, bool negative_one_to_one,
                      struct kk_polygon_vertex out[KK_POLYGON_MAX_VERTICES])
{
   for (unsigned c = 0; c < cull_count; ++c)
      if (cull[0][c] < 0) return 0;
   for (unsigned c = 0; c < 4; ++c) out[0].position[c] = position[0][c];
   for (unsigned c = 0; c < 3; ++c) out[0].weight[c] = c == 0 ? 1.0f : 0.0f;
   for (unsigned plane = 0; plane < 6 + clip_count; ++plane) {
      if (!depth_clip && (plane == 4 || plane == 5)) continue;
      if (kk_polygon_plane_distance(&out[0], plane, clip, negative_one_to_one) < 0)
         return 0;
   }
   return 1;
}
#endif
`));
  edit('src/kosmickrisp/vulkan/kk_polygon_nir.h', s => s.replace(
    '   nir->info.io_lowered = true;', `   nir->info.io_lowered = true;
   if (info->primitive_vertices == 1) {
      nir->info.outputs_written = info->outputs;
      nir_def *id = nir_load_vertex_id(&b);
      nir_def *address = nir_iadd(&b,
         kk_polygon_load(&b, 1, 64, offsetof(struct poly_geometry_capture, output), 8),
         nir_u2u64(&b, nir_imul_imm(&b, id, (util_bitcount64(info->outputs) + 1) * 16)));
      unsigned packed = 0;
      u_foreach_bit64(slot, info->outputs) {
         nir_def *value = nir_load_global_constant(&b, 4, 32,
            nir_iadd_imm(&b, address, packed++ * 16), .align_mul = 16);
         value = nir_convert_to_bit_size(&b, value,
            nir_alu_type_get_base_type(info->output_types[slot]),
            nir_alu_type_get_type_size(info->output_types[slot]));
         nir_store_output(&b, nir_trim_vector(&b, value, info->output_components[slot]),
            nir_imm_int(&b, 0), .src_type = info->output_types[slot],
            .io_semantics.location = slot, .io_semantics.num_slots = 1);
      }
      return nir;
   }`));
  // Retain exact logical bounds, but dispatch full Metal threadgroups. The
  // thread-indirect API does not execute this Poly workload completely.
  edit('src/poly/geometry_capture.h', s => s.replace(
    '   uint32_t clip_grid[6];',
    '   uint32_t clip_grid[6];\n   uint32_t capture_groups[3];\n   uint32_t clip_groups[3];'));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s
    .replace('polygon_grid(global uint *grid, uint x, uint y)',
      'polygon_grid(global uint *grid, global uint *groups, uint x, uint y)')
    .replace('   grid[0] = x;', '   groups[0] = x / 64 + (x % 64 != 0);\n   groups[1] = y;\n   groups[2] = 1;\n   grid[0] = x;')
    .replace('polygon_grid(p->capture_grid,', 'polygon_grid(p->capture_grid, p->capture_groups,')
    .replace('polygon_grid(p->clip_grid,', 'polygon_grid(p->clip_grid, p->clip_groups,')
    .replaceAll('   uint id = cl_global_id.x;',
      '   uint id = cl_global_id.x;\n   if (id >= p->clip_grid[0]) return;')
    .replace('   counts[id] = 0;\n   clipped[id] = 0;',
      '   if (id >= p->clip_grid[0]) return;\n   counts[id] = 0;\n   clipped[id] = 0;'));
  edit('src/kosmickrisp/libkk/kk_conservative.cl', s => s.replaceAll(
    '   uint id = cl_global_id.x;',
    '   uint id = cl_global_id.x;\n   if (id >= p->clip_grid[0]) return;'));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s
    .replaceAll('kk_grid_indirect_local(capture +', 'kk_grid_indirect(capture +')
    .replaceAll('offsetof(struct poly_geometry_capture, capture_grid)',
      'offsetof(struct poly_geometry_capture, capture_groups)')
    .replaceAll('offsetof(struct poly_geometry_capture, clip_grid)',
      'offsetof(struct poly_geometry_capture, clip_groups)'));

  edit('src/poly/geometry_capture.h', s => s.replace(
    '   uint32_t capture_groups[3];',
    '   uint32_t dispatch_groups[3];\n   uint32_t capture_groups[3];'));
  edit('src/kosmickrisp/libkk/kk_polygon.cl', s => s.replace(
    '   p->invocations = geometry->input_primitives;',
    '   p->invocations = geometry->input_primitives;\n   p->dispatch_groups[0] = geometry->grid[0] / 64 + (geometry->grid[0] % 64 != 0);\n   p->dispatch_groups[1] = geometry->grid[1];\n   p->dispatch_groups[2] = 1;'));
  edit('src/kosmickrisp/vulkan/kk_polygon_command.h', s => s.replace(
    '   kk_dispatch_compute(enc, grid, local);',
    '   if (grid.mode == KK_GRID_INDIRECT_LOCAL)\n      grid = kk_grid_indirect(gfx->polygon_capture + offsetof(struct poly_geometry_capture, dispatch_groups));\n   kk_dispatch_compute(enc, grid, local);'));
  edit('src/poly/nir/poly_nir_capture_gs.h', s => s.replace(
    '   nir_shader_intrinsics_pass(shader, lower_id, nir_metadata_control_flow, NULL);\n   return shader;',
    `   nir_shader_intrinsics_pass(shader, lower_id, nir_metadata_control_flow, NULL);
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_cf_list body;
   nir_cf_extract(&body, nir_before_block(nir_start_block(impl)),
                  nir_after_block(nir_impl_last_block(impl)));
   nir_builder guard = nir_builder_at(nir_before_impl(impl));
   nir_def *id = nir_load_global_invocation_id(&guard, 32);
   nir_def *valid = nir_iand(&guard,
      nir_ult(&guard, nir_channel(&guard, id, 0), load_geometry_param(&guard, grid[0])),
      nir_ult(&guard, nir_channel(&guard, id, 1), load_geometry_param(&guard, grid[1])));
   nir_push_if(&guard, valid);
   guard.cursor = nir_cf_reinsert(&body, guard.cursor);
   nir_pop_if(&guard, NULL);
   nir_progress(true, impl, nir_metadata_none);
   return shader;`));

}
