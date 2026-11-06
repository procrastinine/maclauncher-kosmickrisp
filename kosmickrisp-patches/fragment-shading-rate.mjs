// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const sourceFiles = [
  'kosmickrisp-patches/fragment-shading-rate-mesh-prepare.metal',
  'kosmickrisp-patches/fragment-shading-rate-resources.h',
  'kosmickrisp-patches/fragment-shading-rate-resources.c',
  'kosmickrisp-patches/fragment-shading-rate-command.h',
  'kosmickrisp-patches/fragment-shading-rate-command.c',
  'kosmickrisp-patches/fragment-shading-rate-types.h',
  'kosmickrisp-patches/fragment-shading-rate.h',
  'kosmickrisp-patches/fragment-shading-rate.c',
  'kosmickrisp-patches/fragment-shading-rate-bridge.h',
  'kosmickrisp-patches/fragment-shading-rate-bridge.m',
  'kosmickrisp-patches/fragment-shading-rate-pipeline.h',
  'kosmickrisp-patches/fragment-shading-rate-cache.h',
  'kosmickrisp-patches/fragment-shading-rate-cache-impl.h',
];
export function apply({edit, edits}) {
  const owned = {
    'vulkan/kk_fragment_shading_rate_resources.h':'fragment-shading-rate-resources.h',
    'vulkan/kk_fragment_shading_rate_resources.c':'fragment-shading-rate-resources.c',
    'vulkan/kk_fragment_shading_rate_command.h':'fragment-shading-rate-command.h',
    'vulkan/kk_fragment_shading_rate_command.c':'fragment-shading-rate-command.c',
    'libkk/kk_fragment_shading_rate_types.h':'fragment-shading-rate-types.h',
    'compiler/kk_fragment_shading_rate.h':'fragment-shading-rate.h',
    'compiler/kk_fragment_shading_rate.c':'fragment-shading-rate.c',
    'bridge/mtl_fragment_shading_rate.h':'fragment-shading-rate-bridge.h',
    'bridge/mtl_fragment_shading_rate.m':'fragment-shading-rate-bridge.m',
    'vulkan/kk_fragment_shading_rate_pipeline.h':'fragment-shading-rate-pipeline.h',
    'vulkan/kk_fragment_shading_rate_cache.h':'fragment-shading-rate-cache.h',
    'vulkan/kk_fragment_shading_rate_cache_impl.h':'fragment-shading-rate-cache-impl.h',
  };
  for (const [target, source] of Object.entries(owned))
    edits.set('src/kosmickrisp/' + target, fs.readFileSync(new URL('./' + source, import.meta.url), 'utf8'));
  const meshTypes = edits.get('src/kosmickrisp/libkk/kk_mesh_types.h');
  if (!meshTypes) throw new Error('VRS requires shared Mesh job types');
  edits.set('src/kosmickrisp/bridge/mtl_vrs_mesh_types.h',
    '#define KK_VRS_MESH_TYPES_MSL ' + JSON.stringify(meshTypes) + '\n');
  edits.set('src/kosmickrisp/bridge/mtl_vrs_mesh_prepare.h',
    '#define KK_VRS_MESH_PREPARE_MSL ' + JSON.stringify(fs.readFileSync(
      new URL('./fragment-shading-rate-mesh-prepare.metal', import.meta.url), 'utf8')) + '\n');
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => s.replace(
    '#include "kk_unused_attachments.h"', '#include "kk_unused_attachments.h"\n#include "kk_fragment_shading_rate_pipeline.h"'));
  edit('src/kosmickrisp/vulkan/kk_device.h', (s, replace) => replace(
    '   struct kk_dgc_metal *dgc;',
    '   struct kk_vrs_device *vrs;\n   struct kk_dgc_metal *dgc;'));
  edit('src/kosmickrisp/vulkan/kk_device.c', (s, replace) => {
    s = s.replace('#include "kk_shader.h"', '#include "kk_shader.h"\n#include "kk_fragment_shading_rate_cache.h"');
    s = s.replace('   kk_descriptor_heap_init(dev);',
      '   kk_descriptor_heap_init(dev);\n   result = kk_vrs_device_init(dev);\n   if (result != VK_SUCCESS) goto fail_init;');
    s = s.replace('fail_init:\n', 'fail_init:\n   kk_vrs_device_finish(dev);\n');
    s = s.replace('   kk_interlock_device_finish(dev);',
      '   kk_interlock_device_finish(dev);\n   kk_vrs_device_finish(dev);');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_shader.h', (s, replace) => {
    s = s.replace('   KK_FEAT_COUNT,', '   KK_FEAT_FRAGMENT_SHADING_RATE,\n   KK_FEAT_COUNT,');
    s = s.replace('   struct kk_sparse_shader_state *sparse;',
      '   struct kk_sparse_shader_state *sparse;\n   struct kk_vrs_source *vrs_source;');
    return s.replace('#include "vk_pipeline_cache.h"',
      '#include "vk_pipeline_cache.h"\n#include "kk_fragment_shading_rate_cache.h"');
  });
  edit('src/kosmickrisp/vulkan/kk_shader.c', (s, replace) => {
    s = s.replace('   kk_feature_key key = 0;',
      '   kk_feature_key key = 0;\n   if (feats->pipelineFragmentShadingRate || feats->primitiveFragmentShadingRate ||\n       feats->attachmentFragmentShadingRate)\n      key |= BITFIELD_BIT(KK_FEAT_FRAGMENT_SHADING_RATE);');
    s = s.replace('   vk_shader_free(&dev->vk, pAllocator, &shader->vk);',
      '   kk_vrs_source_destroy(dev, shader->vrs_source);\n   vk_shader_free(&dev->vk, pAllocator, &shader->vk);');
    s = s.replace('static VkResult\nkk_deserialize_shader_native(',
      '#include "kk_fragment_shading_rate_cache_impl.h"\n\nstatic VkResult\nkk_deserialize_shader_native(');
    s = s.replace('   return !blob->out_of_memory;\n}',
      '   if (vk_dev->enabled_features.pipelineFragmentShadingRate ||\n       vk_dev->enabled_features.primitiveFragmentShadingRate ||\n       vk_dev->enabled_features.attachmentFragmentShadingRate)\n      kk_vrs_source_serialize(shader->vrs_source, blob);\n   return !blob->out_of_memory;\n}');
    s = s.replace('   if (info.stage == MESA_SHADER_COMPUTE) {',
      '   if (dev->vrs) {\n      result = kk_vrs_source_deserialize(dev, blob, &shader->vrs_source);\n      if (result != VK_SUCCESS) goto fail;\n   }\n   if (info.stage == MESA_SHADER_COMPUTE) {');
    return s;
  });
  edit('src/kosmickrisp/vulkan/meson.build', s => s.replace(
    "  'kk_cmd_draw.c',", "  'kk_cmd_draw.c',\n  'kk_fragment_shading_rate_command.c',\n  'kk_fragment_shading_rate_resources.c',"));
  linkedFragmentPhase({edit});
  retainedLinkedSource({edit});
  edit('src/kosmickrisp/compiler/meson.build', s => s.replace(
    "    'nir_to_msl.c',", "    'nir_to_msl.c',\n    'kk_fragment_shading_rate.c',"));
  edit('src/kosmickrisp/bridge/meson.build', s => s.replace(
    "    'mtl_encoder.m',", "    'mtl_encoder.m',\n    'mtl_fragment_shading_rate.m',"));
}

function linkedFragmentPhase({edit}) {
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    const lower = s.indexOf('static void\nkk_lower_nir(');
    const start = s.indexOf('   if (features & BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD))', lower);
    const end = s.indexOf('\n}\n\nstatic const struct vk_shader_ops', start);
    if (lower < 0 || start < 0 || end < 0) throw new Error('VRS descriptor-tail anchors');
    const tail = s.slice(start, end);
    const helper = `static void
kk_finish_nir(struct kk_device *dev, nir_shader *nir,
              const struct vk_pipeline_robustness_state *rs,
              uint32_t set_layout_count,
              struct vk_descriptor_set_layout *const *set_layouts,
              kk_feature_key features)
{
   struct kk_physical_device *pdev = kk_device_physical(dev);
${tail}
}

`;
    s = s.replace(tail, `   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      kk_finish_nir(dev, nir, rs, set_layout_count, set_layouts, features);`);
    s = s.replace('static void\nkk_lower_nir(', helper + 'static void\nkk_lower_nir(');
    s = s.replace('      kk_lower_fs(dev, nir, state);',
      '      NIR_PASS(_, nir, msl_nir_fs_io_types);');
    s = s.replace('   NIR_PASS(_, nir, msl_nir_fs_io_types);\n\n   if (kk_fs_has_sample_mask(state))',
      '   if (kk_fs_has_sample_mask(state))');
    s = s.replace('   nir_shader *conservative_fs = kk_conservative_possible(state)', `   /* Link original inputs before the fine epilogue; its complete native
    * inputs must exist before the one mesh/per-vertex packing decision. */
   for (unsigned i = 0; i < total_shaders; i++) {
      nir_shader *fs = nir_shaders[i];
      if (fs->info.stage != MESA_SHADER_FRAGMENT) continue;
      const struct vk_shader_compile_info *info = NULL;
      for (unsigned j = 0; j < shader_count; j++)
         if (infos[j].stage == MESA_SHADER_FRAGMENT) info = &infos[j];
      kk_lower_fs(dev, fs, state);
      kk_finish_nir(dev, fs, info ? info->robustness : &rs_none,
         info ? info->set_layout_count : 0, info ? info->set_layouts : NULL, features);
      nir_opts(fs, NULL);
      nir_shader_gather_info(fs, nir_shader_get_entrypoint(fs));
   }

   nir_shader *conservative_fs = kk_conservative_possible(state)`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c', s => {
    s = s.replaceAll('nir_foreach_block(block, nir_shader_get_entrypoint(fs)) {',
      'nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {');
    s = s.replace('gather_flat_locations(nir_function_impl *impl, struct kk_per_vertex_map *map)',
      'gather_flat_locations(nir_shader *fs, struct kk_per_vertex_map *map)');
    const flat = s.indexOf('gather_flat_locations(nir_shader *fs');
    const flat_end = s.indexOf('\n}\n', flat);
    const flat_body = s.slice(flat, flat_end);
    s = s.replace(flat_body, flat_body.replace('nir_foreach_block(block, impl) {',
      'nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {'));
    const lower = s.indexOf('kk_nir_lower_fs_per_vertex_inputs(nir_shader *fs');
    const end = s.indexOf('\n}\n', lower);
    const original = s.slice(lower, end);
    let body = original.replace('   nir_foreach_block(block, impl) {',
      '   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {');
    body = body.replace('   nir_builder b = nir_builder_create(impl);\n   nir_foreach_block(block, impl) {',
      '   nir_builder b = nir_builder_create(impl);\n   nir_foreach_function_impl(body, fs) nir_foreach_block(block, body) {\n      b.impl = body;');
    body = body.replace('   gather_flat_locations(impl, map);', '   gather_flat_locations(fs, map);');
    body = body.replace('   nir_progress(true, impl, nir_metadata_control_flow);',
      '   nir_foreach_function_impl(body, fs)\n      nir_progress(true, body, nir_metadata_control_flow);');
    s = s.replace(original, body);
    return s;
  });
}

function retainedLinkedSource({edit}) {
  edit('src/kosmickrisp/vulkan/kk_compile_options.h', s => s.replace(
    '   struct nir_shader *conservative_fs;',
    '   struct nir_shader *conservative_fs;\n   struct nir_shader *vrs_source;'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.h', s => s.replace(
    'bool kk_nir_lower_fs_per_vertex_inputs(',
    'bool kk_nir_apply_fs_per_vertex_map(nir_shader *fs, struct kk_per_vertex_map *map);\n\nbool kk_nir_lower_fs_per_vertex_inputs('));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c', s => {
    const start = s.indexOf('kk_nir_lower_fs_per_vertex_inputs(nir_shader *fs');
    const begin = s.indexOf('   nir_builder b = nir_builder_create(impl);', start);
    const end = s.indexOf('\n}\n', begin);
    if (start < 0 || begin < 0 || end < 0) throw new Error('VRS actual-map apply anchors');
    const body = s.slice(begin, end);
    s = s.replace(body, '   return kk_nir_apply_fs_per_vertex_map(fs, map);');
    s = s.replace('bool\nkk_nir_lower_fs_per_vertex_inputs(', `bool
kk_nir_apply_fs_per_vertex_map(nir_shader *fs, struct kk_per_vertex_map *map)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(fs);
${body}
}

bool
kk_nir_lower_fs_per_vertex_inputs(`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('   nir_shader *nir_shaders[shader_count + 2u];',
      '   nir_shader *nir_shaders[shader_count + 2u];\n   nir_shader *vrs_sources[shader_count + 2u];\n   memset(vrs_sources, 0, sizeof(vrs_sources));\n   nir_shader *fs_to_finish = NULL;');
    s = s.replace('      kk_lower_nir(dev, nir, emulated_stage, info->robustness,',
      '      if (nir->info.stage == MESA_SHADER_FRAGMENT && !nir->info.io_lowered)\n         fs_to_finish = nir;\n      kk_lower_nir(dev, nir, emulated_stage, info->robustness,');
    s = s.replace('      nir_shaders[total_shaders] = get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);',
      '      nir_shaders[total_shaders] = get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);\n      fs_to_finish = nir_shaders[total_shaders];');
    s = s.replace('      if (fs->info.stage != MESA_SHADER_FRAGMENT) continue;',
      '      if (fs->info.stage != MESA_SHADER_FRAGMENT || fs != fs_to_finish) continue;');
    s = s.replace('      kk_lower_fs(dev, fs, state);', `      /* One callable program receives the fine epilogue and descriptor tail.
       * Retention copies only finished source; compilation flattens it once. */
      bool callable = kk_nir_vrs_split_application(fs);
      kk_lower_fs(dev, fs, state);`);
    s = s.replace('      nir_opts(fs, NULL);', `      if (dev->vrs && callable && (!info || !info->embedded_sampler_count))
         vrs_sources[i] = nir_shader_clone(NULL, fs);
      kk_nir_vrs_flatten(fs);
      kk_nir_vrs_lower_fine_rate(fs);
      nir_opts(fs, NULL);`);
    s = s.replace('   const struct kk_compile_options compile_options = {', `   for (unsigned i = 0; i < total_shaders; i++) {
      if (vrs_sources[i] && per_vertex_map.active) {
         struct kk_per_vertex_map retained_map = per_vertex_map;
         kk_nir_apply_fs_per_vertex_map(vrs_sources[i], &retained_map);
      }
   }
   const struct kk_compile_options compile_options = {`);
    s = s.replace('      result =\n         kk_compile_shader(dev, nir_shaders[i], prev_stage, vertex_robustness,\n                           state, &compile_options, pAllocator, &shaders[i]);',
      '      struct kk_compile_options stage_options = compile_options;\n      stage_options.vrs_source = vrs_sources[i];\n      vrs_sources[i] = NULL;\n      result =\n         kk_compile_shader(dev, nir_shaders[i], prev_stage, vertex_robustness,\n                           state, &stage_options, pAllocator, &shaders[i]);');
    s = s.replace('         for (uint32_t j = i + 1; j < total_shaders; j++)\n            ralloc_free(nir_shaders[j]);',
      '         for (uint32_t j = i + 1; j < total_shaders; j++) {\n            ralloc_free(nir_shaders[j]);\n            ralloc_free(vrs_sources[j]);\n         }');
    s = s.replace('      for (uint32_t i = 0; i < total_shaders; i++)\n         ralloc_free(nir_shaders[i]);',
      '      for (uint32_t i = 0; i < total_shaders; i++) {\n         ralloc_free(nir_shaders[i]);\n         ralloc_free(vrs_sources[i]);\n      }');
    s = s.replace('   if (shader == NULL) {\n      ralloc_free(nir);',
      '   if (shader == NULL) {\n      ralloc_free(opts->vrs_source);\n      ralloc_free(nir);');
    s = s.replace('   *shader_out = shader;\n\n   return result;', `   if (opts->vrs_source) {
      shader->vrs_source = kk_vrs_source_capture(dev, opts->vrs_source,
         per_vertex_map, state, &shader->msl_data[stage]);
      ralloc_free(opts->vrs_source);
      if (!shader->vrs_source) {
         kk_shader_destroy(&dev->vk, shader, pAllocator);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   *shader_out = shader;

   return result;`);
    return s;
  });
}
