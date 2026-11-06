// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import {apply as applySlots} from './compile-shader-options-slots.mjs';

export const id = 'compile-shader-options';
export const sourceFiles = ['kosmickrisp-compile-options.h',
  'compile-shader-options-slots.mjs', 'kosmickrisp-argument-table.h']
  .map(file => 'kosmickrisp-patches/' + file);

/** Keep shader compilation inputs in one shared options structure. */
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_compile_options.h',
    fs.readFileSync(new URL('./kosmickrisp-compile-options.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s.replace(
    '#include "kk_private.h"', '#include "kk_private.h"\n#include "kk_compile_options.h"'));
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => s.replace(
    '   uint64_t disabled_workarounds;',
    '   uint64_t disabled_workarounds;\n   bool mesh_queries;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('kk_compile_msl(struct kk_physical_device *pdev, nir_shader *nir,',
      'kk_compile_msl_options(struct kk_physical_device *pdev, nir_shader *nir,');
    s = s.replace('               struct msl_compile_data *data)\n{',
      '               struct msl_compile_data *data,\n               const struct kk_compile_options *opts)\n{');
    s = s.replace(`   struct nir_to_msl_options translate_options = {
      .mem_ctx = NULL,
      .disabled_workarounds = pdev->settings.disabled_workarounds,
   };`, `   struct nir_to_msl_options translate_options = opts && opts->msl_custom
      ? *opts->msl_custom : (struct nir_to_msl_options){0};
   translate_options.mem_ctx = NULL;
   translate_options.disabled_workarounds = pdev->settings.disabled_workarounds;
   translate_options.mesh_queries = opts && opts->mesh_queries;`);
    s = s.replace('#include "kk_polygon_nir.h"', `static void
kk_compile_msl(struct kk_physical_device *pdev, nir_shader *nir,
               const struct vk_graphics_pipeline_state *state,
               struct msl_compile_data *data)
{
   kk_compile_msl_options(pdev, nir, state, data, NULL);
}

#include "kk_polygon_nir.h"`);
    s = s.replace('                  const struct kk_per_vertex_map *per_vertex_map,',
      '                  const struct kk_compile_options *opts,');
    s = s.replace('   assert(nir->info.io_lowered && "nir must have lowered io");',
      `   assert(nir->info.io_lowered && "nir must have lowered io");
   static const struct kk_compile_options defaults;
   if (!opts)
      opts = &defaults;
   const struct kk_per_vertex_map *per_vertex_map = opts->per_vertex_map;`);
    s = s.replace('   kk_compile_msl(pdev, nir, state, &shader->msl_data[stage]);',
      '   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts);');
    s = s.replace(`   for (uint32_t i = 0; i < total_shaders; i++) {
      struct kk_shader *prev_stage = i > 0 ? shaders[i - 1] : NULL;`,
      `   const struct kk_compile_options compile_options = {
      .per_vertex_map = &per_vertex_map,
   };
   for (uint32_t i = 0; i < total_shaders; i++) {
      struct kk_shader *prev_stage = i > 0 ? shaders[i - 1] : NULL;`);
    s = s.replace('                           state, &per_vertex_map, pAllocator, &shaders[i]);',
      '                           state, &compile_options, pAllocator, &shaders[i]);');
    return s;
  });
  applySlots({edit, edits});
}
