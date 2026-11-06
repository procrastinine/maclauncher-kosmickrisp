// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';
export const sourceFiles = ["kosmickrisp-patches/kosmickrisp-polygon-types.h","kosmickrisp-patches/kosmickrisp-poly-capture.h","kosmickrisp-patches/kosmickrisp-polygon.h","kosmickrisp-patches/kosmickrisp-polygon.cl","kosmickrisp-patches/kosmickrisp-polygon-nir.h","kosmickrisp-patches/kosmickrisp-polygon-command.h"];
export function apply({edit, read, edits}) {
const owned = name => fs.readFileSync(path.join(import.meta.dirname, name), 'utf8');
  edits.set('src/poly/geometry_capture.h', owned('kosmickrisp-polygon-types.h'));
  edits.set('src/kosmickrisp/libkk/kk_polygon.h', owned('kosmickrisp-polygon.h'));
  edits.set('src/kosmickrisp/libkk/kk_polygon.cl', owned('kosmickrisp-polygon.cl'));
  edits.set('src/kosmickrisp/vulkan/kk_polygon_nir.h', owned('kosmickrisp-polygon-nir.h'));
  edits.set('src/kosmickrisp/vulkan/kk_polygon_command.h', owned('kosmickrisp-polygon-command.h'));
  edit('src/kosmickrisp/libkk/meson.build', s => s.replace("  'kk_geometry.cl',", "  'kk_geometry.cl',\n  'kk_polygon.cl',"));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
    .replace('      .fullDrawIndexUint32 = true,',
             '      .fullDrawIndexUint32 = true,\n      .fillModeNonSolid = true,')
    .replace('      .extendedDynamicState3LineRasterizationMode = true,',
             '      .extendedDynamicState3LineRasterizationMode = true,\n      .extendedDynamicState3PolygonMode = true,'));
  const lower = read('src/poly/nir/poly_nir_lower_gs.c');
  const rast = lower.slice(lower.indexOf('create_gs_rast_shader('));
  const xfb = rast.slice(rast.indexOf('   if (gs->xfb_info) {'),
                         rast.indexOf('   /* Forward each selected output'));
  if (!xfb.includes('poly_xfb_vertex_copy_in_strip'))
    throw new Error('Missing original Poly transform-feedback lowering');
  edits.set('src/poly/nir/poly_nir_capture_gs.h',
    owned('kosmickrisp-poly-capture.h').replace('   /* POLY_CAPTURE_XFB_BODY */', xfb));

  edit('src/poly/geometry.h', s => s
    .replace('   uint32_t input_topology;\n} PACKED;',
      '   uint32_t input_topology;\n   uint32_t capture_pad;\n   uint64_t capture;\n} PACKED;')
    .replace('sizeof(struct poly_geometry_params) == 79 * 4',
             'sizeof(struct poly_geometry_params) == 82 * 4')
    .replace('struct poly_geometry_params must be 79 words',
             'struct poly_geometry_params must be 82 words'));
  edit('src/poly/nir/poly_nir.h', s => s
    .replace('#include "poly/geometry.h"',
             '#include "poly/geometry.h"\n#include "poly/geometry_capture.h"')
    .replace('bool poly_nir_lower_tcs(', `bool poly_nir_lower_gs_capture(struct nir_shader *gs,
   struct nir_shader **gs_count, struct nir_shader **gs_copy,
   struct nir_shader **pre_gs, struct poly_gs_info *info,
   struct poly_gs_capture_info *capture_info,
   struct nir_shader **capture, struct nir_shader **capture_xfb);

bool poly_nir_lower_tcs(`));
  edit('src/poly/nir/poly_nir_lower_gs.c', s => {
    const definition = `bool
poly_nir_lower_gs(nir_shader *gs, nir_shader **gs_count, nir_shader **gs_copy,
                  nir_shader **pre_gs, struct poly_gs_info *info)`;
    if (!s.includes(definition)) throw new Error('Missing original Poly GS entry point');
    s = s.replace(definition, `#include "poly_nir_capture_gs.h"

bool
poly_nir_lower_gs_capture(nir_shader *gs, nir_shader **gs_count,
   nir_shader **gs_copy, nir_shader **pre_gs, struct poly_gs_info *info,
   struct poly_gs_capture_info *capture_info,
   nir_shader **capture, nir_shader **capture_xfb)`);
    s = s.replace('   *gs_copy = create_gs_rast_shader(gs, &gs_state);',
      `   if (capture) {
      *capture = create_gs_capture_shader(gs, &gs_state, capture_info);
      *capture_xfb = create_gs_capture_xfb_shader(gs, &gs_state);
   }
   *gs_copy = create_gs_rast_shader(gs, &gs_state);`);
    return s + `
bool
poly_nir_lower_gs(nir_shader *gs, nir_shader **gs_count,
   nir_shader **gs_copy, nir_shader **pre_gs, struct poly_gs_info *info)
{
   return poly_nir_lower_gs_capture(gs, gs_count, gs_copy, pre_gs, info,
                                   NULL, NULL, NULL);
}
`;
  });

  edit('src/kosmickrisp/compiler/msl_iomap.c', s => s.replace(
    '   [VARYING_SLOT_POS] = {"position"},',
    '   [VARYING_SLOT_POS] = {"position"},\n   [VARYING_SLOT_PNTC] = {"polygon_point_coord", .user = true},'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
    .replace('   bool has_geometry;', `   bool has_geometry;
   bool polygon_point;
   struct poly_gs_capture_info polygon;`)
    .replace('         mtl_render_pipeline_state *render;', `         mtl_render_pipeline_state *render;
         mtl_render_pipeline_state *polygon_render;
         mtl_compute_pipeline_state *polygon_capture;
         mtl_compute_pipeline_state *polygon_xfb;`)
    .replace('         uint64_t flat_outputs;', '         uint64_t flat_outputs;\n         uint64_t noperspective_outputs;')
    .replace('   struct msl_compile_data gs_main;', `   struct msl_compile_data gs_main;
   struct msl_compile_data polygon[4];`));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('static VkResult\nkk_compile_shader(', '#include "kk_polygon_nir.h"\n\nstatic VkResult\nkk_compile_shader(');
    s = s.replace('   gather_shader_info(shader, nir, state);', `   gather_shader_info(shader, nir, state);
   if (stage == MESA_SHADER_FRAGMENT && kk_polygon_possible(state)) {
      shader->info.fs.noperspective_outputs = kk_polygon_noperspective_outputs(nir);
      nir_shader *point_fs = nir_shader_clone(NULL, nir);
      nir_shader_intrinsics_pass(point_fs, kk_polygon_point_coord,
                                 nir_metadata_control_flow, NULL);
      shader->info.polygon_point = true;
      kk_compile_msl(pdev, point_fs, state, &shader->polygon[3]);
   }`);
    s = s.replace('      nir_shader *count = NULL, *rast = NULL, *pre_gs = NULL;\n      NIR_PASS(_, nir, poly_nir_lower_gs, &count, &rast, &pre_gs,\n               &shader->info.gs);',
      `      nir_shader *count = NULL, *rast = NULL, *pre_gs = NULL;
      nir_shader *capture = NULL, *capture_xfb = NULL;
      bool polygon = kk_polygon_possible(state) &&
         u_reduced_prim(nir->info.gs.output_primitive) == MESA_PRIM_TRIANGLES;
      NIR_PASS(_, nir, poly_nir_lower_gs_capture, &count, &rast, &pre_gs,
               &shader->info.gs, &shader->info.polygon,
               polygon ? &capture : NULL, polygon ? &capture_xfb : NULL);
      if (polygon) {
         shader->info.polygon_point = true;
         nir_shader *point_vs = kk_polygon_vertex_shader(&shader->info.polygon, nir->options);
         NIR_PASS(_, point_vs, msl_nir_vs_io_types);
         NIR_PASS(_, point_vs, kk_nir_lower_descriptors, robustness, 0, NULL);
         kk_compile_msl(pdev, point_vs, state, &shader->polygon[2]);
         nir_shader *programs[] = {capture, capture_xfb};
         for (unsigned i = 0; i < ARRAY_SIZE(programs); ++i) {
            if (!programs[i]) continue;
            programs[i]->info.stage = MESA_SHADER_COMPUTE;
            memset(&programs[i]->info.cs, 0, sizeof(programs[i]->info.cs));
            programs[i]->info.workgroup_size[0] = KK_POLY_WG_SIZE;
            programs[i]->info.workgroup_size[1] = programs[i]->info.workgroup_size[2] = 1;
            msl_preprocess_nir(programs[i]);
            kk_compile_msl(pdev, programs[i], state, &shader->polygon[i]);
         }
      }`);
    s = s.replace('      emulated_stage |= nir->xfb_info && (nir->info.stage == MESA_SHADER_VERTEX ||',
      '      emulated_stage |= (nir->xfb_info || kk_polygon_possible(state)) && (nir->info.stage == MESA_SHADER_VERTEX ||');
    s = s.replace('   if (!explicit_geometry && xfb) {', '   if (!explicit_geometry && (xfb || kk_polygon_possible(state))) {');
    s = s.replace('         if (nir_shaders[i]->xfb_info) producer = nir_shaders[i];',
      '         if (nir_shaders[i]->info.stage != MESA_SHADER_FRAGMENT) producer = nir_shaders[i];');
    s = s.replace('      size_t key_size = poly_passthrough_gs_key_size(producer->xfb_info->output_count);',
      '      size_t key_size = poly_passthrough_gs_key_size(producer->xfb_info ? producer->xfb_info->output_count : 0);');
    s = s.replace(`      memcpy(&key->xfb_info, producer->xfb_info,
             nir_xfb_info_size(producer->xfb_info->output_count));`,
      `      if (producer->xfb_info)
         memcpy(&key->xfb_info, producer->xfb_info,
                nir_xfb_info_size(producer->xfb_info->output_count));`);
    s = s.replace('      poly_nir_passthrough_gs(&builder, key);',
      `      poly_nir_passthrough_gs(&builder, key);
      if (kk_polygon_possible(state))
         nir_shader_intrinsics_pass(builder.shader, kk_polygon_passthrough_primitive_id,
                                    nir_metadata_control_flow, NULL);`);
    s = s.replace('         kk_msl_copy(&vs->gs_main, &s->gs_main);', `         kk_msl_copy(&vs->gs_main, &s->gs_main);
         info->polygon_point = s->info.polygon_point;
         info->polygon = s->info.polygon;
         if (info->polygon_point) {
            for (unsigned j = 0; j < 3; ++j)
               if (s->polygon[j].code) kk_msl_copy(&vs->polygon[j], &s->polygon[j]);
         }`);
    s = s.replace('      info->vs.additional_stages_bits |= BITFIELD_BIT(stage);',
      `      info->vs.additional_stages_bits |= BITFIELD_BIT(stage);
      if (stage == MESA_SHADER_FRAGMENT && info->polygon_point)
         kk_msl_copy(&vs->polygon[3], &s->polygon[3]);`);

    // Each variant has the same attachments and sample state, with its own
    // vertex/fragment functions. Pre-raster programs are shared and compiled once.
    s = s.replace('kk_compile_graphics_pipeline(struct kk_device *device, struct kk_shader *vs)',
      'kk_compile_graphics_pipeline_native(struct kk_device *device, struct kk_shader *vs, bool polygon)');
    s = s.replace('   uint32_t stages_bits = vs->info.vs.additional_stages_bits;',
      '   uint32_t stages_bits = vs->info.vs.additional_stages_bits;\n   if (!polygon) {');
    s = s.replace('   const struct msl_compile_data *vs_data = &vs->msl_data[vs_stage];',
      '   }\n   const struct msl_compile_data *vs_data = polygon ? &vs->polygon[2] : &vs->msl_data[vs_stage];');
    s = s.replace('   const struct msl_compile_data *fs_data = &vs->msl_data[MESA_SHADER_FRAGMENT];',
      '   const struct msl_compile_data *fs_data = polygon ? &vs->polygon[3] : &vs->msl_data[MESA_SHADER_FRAGMENT];');
    s = s.replace('      pipeline_descriptor, vs->info.vs.topology);',
      '      pipeline_descriptor, polygon ? MTL_PRIMITIVE_TOPOLOGY_CLASS_TRIANGLE : vs->info.vs.topology);');
    s = s.replace('   if (vs->info.vs.has_ds) {', '   if (vs->info.vs.has_ds && !polygon) {');
    s = s.replace(`   pipe->gfx.render =
      mtl_new_render_pipeline(device->mtl_compiler_handle, pipeline_descriptor);
   if (pipe->gfx.render == NULL)`, `   mtl_render_pipeline_state *render =
      mtl_new_render_pipeline(device->mtl_compiler_handle, pipeline_descriptor);
   if (polygon) pipe->gfx.polygon_render = render;
   else pipe->gfx.render = render;
   if (render == NULL)`);
    const end = s.indexOf('static bool\nmove_primitive_id');
    if (end < 0) throw new Error('Missing graphics pipeline boundary');
    s = s.slice(0, end) + `static VkResult
kk_compile_graphics_pipeline(struct kk_device *device, struct kk_shader *vs)
{
   VkResult result = kk_compile_graphics_pipeline_native(device, vs, false);
   if (result != VK_SUCCESS || !vs->info.polygon_point) return result;
   result = kk_compile_compute_pipeline(device, &vs->polygon[0], KK_POLY_WG_SIZE,
                                        &vs->pipeline.gfx.polygon_capture);
   if (result != VK_SUCCESS) return result;
   if (vs->polygon[1].code) {
      result = kk_compile_compute_pipeline(device, &vs->polygon[1], KK_POLY_WG_SIZE,
                                           &vs->pipeline.gfx.polygon_xfb);
      if (result != VK_SUCCESS) return result;
   }
   return kk_compile_graphics_pipeline_native(device, vs, true);
}

` + s.slice(end);
    s = s.replace('   ralloc_free((void *)shader->gs_main.entrypoint_name);', `   if (shader->info.stage == MESA_SHADER_VERTEX) {
      mtl_release(pipe->gfx.polygon_render);
      mtl_release(pipe->gfx.polygon_capture);
      mtl_release(pipe->gfx.polygon_xfb);
   }
   for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i) {
      ralloc_free(shader->polygon[i].entrypoint_name);
      ralloc_free(shader->polygon[i].code);
   }
   ralloc_free((void *)shader->gs_main.entrypoint_name);`);
    s = s.replace('   return !blob->out_of_memory;', `   if (shader->info.polygon_point) {
      for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i) {
         blob_write_uint8(blob, shader->polygon[i].code != NULL);
         if (shader->polygon[i].code) kk_msl_serialize(&shader->polygon[i], blob);
      }
   }
   return !blob->out_of_memory;`);
    s = s.replace('   if (info.stage == MESA_SHADER_COMPUTE) {', `   if (info.polygon_point) {
      for (unsigned i = 0; i < ARRAY_SIZE(shader->polygon); ++i) {
         if (!blob_read_uint8(blob)) continue;
         result = kk_msl_deserialize(blob, &shader->polygon[i]);
         if (result != VK_SUCCESS) goto fail;
      }
   }
   if (info.stage == MESA_SHADER_COMPUTE) {`);
    return s;
  });

  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    'struct kk_graphics_state {',
    'struct kk_graphics_state {\n   bool polygon_points;\n   uint64_t polygon_capture;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
    s = s.replace('   mtl_set_viewports(encoder, viewports, count);', `   if (cmd->state.gfx.polygon_points) {
      const struct kk_rendering_state *render = &cmd->state.gfx.render;
      double width = MAX2(render->area.offset.x + render->area.extent.width, 1);
      double height = MAX2(render->area.offset.y + render->area.extent.height, 1);
      for (unsigned i = 0; i < count; ++i) {
         viewports[i] = (struct mtl_viewport){
            .originX = 0, .originY = height, .width = width, .height = -height,
            .znear = 0, .zfar = 1,
         };
      }
   }
   mtl_set_viewports(encoder, viewports, count);`);
    s = s.replace('   if (IS_SHADER_DIRTY(VERTEX)) {',
      '   if (IS_SHADER_DIRTY(VERTEX) || IS_DIRTY(RS_POLYGON_MODE)) {');
    s = s.replace('      mtl_render_set_pipeline_state(enc, vs->pipeline.gfx.render);',
      '      mtl_render_set_pipeline_state(enc, gfx->polygon_points ? vs->pipeline.gfx.polygon_render : vs->pipeline.gfx.render);');
    s = s.replace('                           vk_front_face_to_mtl_cull_mode(dyn->rs.cull_mode));',
      '                           gfx->polygon_points ? MTL_CULL_MODE_NONE : vk_front_face_to_mtl_cull_mode(dyn->rs.cull_mode));');
    s = s.replace('mtl_set_triangle_fill_lines(enc, dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE);',
      'mtl_set_triangle_fill_lines(enc, !gfx->polygon_points && dyn->rs.polygon_mode == VK_POLYGON_MODE_LINE);');
    s = s.replace('      if (dyn->rs.depth_bias.enable)',
      '      if (dyn->rs.depth_bias.enable && !gfx->polygon_points)');
    s = s.replace('         clip ? MTL_DEPTH_CLIP_MODE_CLIP : MTL_DEPTH_CLIP_MODE_CLAMP;',
      '         clip && !gfx->polygon_points ? MTL_DEPTH_CLIP_MODE_CLIP : MTL_DEPTH_CLIP_MODE_CLAMP;');

    const upload = '   return kk_pool_upload(cmd, &params, sizeof(params), 8).gpu;';
    if (!s.includes(upload)) throw new Error('Missing Poly geometry parameter upload');
    const geometryUpload = s.indexOf('kk_upload_geometry_params(');
    s = s.slice(0, geometryUpload) + s.slice(geometryUpload).replace(upload, `   if (gfx->polygon_points) {
      const struct poly_gs_capture_info *capture = &gs->info.polygon;
      const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
      struct poly_geometry_capture point = {
         .outputs = capture->outputs,
         .float_outputs = capture->float_outputs,
         .noperspective_outputs = fs ? fs->info.fs.noperspective_outputs : 0,
         .vertex_stride = util_bitcount64(capture->outputs) * 16,
         .vertices_out = capture->vertices_out,
         .streams = capture->streams,
         .clip_count = capture->clip_count,
         .cull_count = capture->cull_count,
         .cull_mode = dyn->rs.cull_mode,
         .front_ccw = dyn->rs.front_face != VK_FRONT_FACE_CLOCKWISE,
         .depth_clip = vk_rasterization_state_depth_clip_enable(&dyn->rs),
         .depth_clamp = dyn->rs.depth_clamp_enable,
         .negative_one_to_one = dyn->vp.depth_clip_negative_one_to_one,
         .provoking_last = dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT,
         .rasterization_stream = dyn->rs.rasterization_stream,
         .depth_bias_constant = dyn->rs.depth_bias.constant_factor,
         .depth_bias_slope = dyn->rs.depth_bias.slope_factor,
         .depth_bias_clamp = dyn->rs.depth_bias.clamp,
         .depth_bias_enable = dyn->rs.depth_bias.enable,
         .depth_mantissa_bits = 23, .depth_is_float = true,
         .render_size = {
            MAX2(gfx->render.area.offset.x + gfx->render.area.extent.width, 1),
            MAX2(gfx->render.area.offset.y + gfx->render.area.extent.height, 1)},
      };
      if (cmd->state.shaders[MESA_SHADER_TESS_EVAL]) {
         point.front_ccw ^= gfx->tess.info.ccw;
         point.front_ccw ^= dyn->ts.domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
      }
      VkFormat depth = gfx->render.depth_att.vk_format;
      if (depth == VK_FORMAT_D16_UNORM || depth == VK_FORMAT_D16_UNORM_S8_UINT) {
         point.depth_mantissa_bits = 16; point.depth_is_float = false;
      } else if (depth == VK_FORMAT_X8_D24_UNORM_PACK32 || depth == VK_FORMAT_D24_UNORM_S8_UINT) {
         point.depth_mantissa_bits = 24; point.depth_is_float = false;
      }
      for (unsigned i = 0; i < dyn->vp.viewport_count; ++i) {
         const VkViewport *vp = &dyn->vp.viewports[i];
         point.viewport[i][0] = vp->x; point.viewport[i][1] = vp->y;
         point.viewport[i][2] = vp->width; point.viewport[i][3] = vp->height;
         point.viewport[i][4] = vp->minDepth; point.viewport[i][5] = vp->maxDepth;
      }
      params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;
   }
   return kk_pool_upload(cmd, &params, sizeof(params), 8).gpu;`);
    s = s.replace('/* Runs the stage feeding the GS and the GS compute pass, returning the draw',
      '#include "kk_polygon_command.h"\n\n/* Runs the stage feeding the GS and the GS compute pass, returning the draw');
    s = s.replace('   kk_dispatch_compute(enc, grid_vs, local_size);\n\n   bool queries = false;',
      `   kk_dispatch_compute(enc, grid_vs, local_size);
   if (gfx->polygon_points)
      return kk_launch_polygon_points(cmd, grid_gs, geometry_params);

   bool queries = false;`);
    s = s.replace(`kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;`,
      `kk_draw(struct kk_cmd_buffer *cmd, struct kk_draw_command *data)
{
   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   bool point = vs->info.polygon_point && dyn->rs.polygon_mode == VK_POLYGON_MODE_POINT;
   if (point != gfx->polygon_points) {
      gfx->polygon_points = point;
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_POLYGON_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_CULL_MODE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_VP_VIEWPORTS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_FACTORS);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_DEPTH_CLIP_ENABLE);
      BITSET_SET(dyn->dirty, MESA_VK_DYNAMIC_RS_FRONT_FACE);
   }`);
    return s;
  });
}
