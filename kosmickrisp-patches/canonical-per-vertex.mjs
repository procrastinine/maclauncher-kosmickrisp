// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export const id = 'canonical-per-vertex';
export const sourceFiles = ['kosmickrisp-patches/canonical-per-vertex.h'];

/** Independent, non-LTO stages share one fixed primitive-corner interface. */
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/vulkan/kk_canonical_per_vertex.h',
    fs.readFileSync(new URL('./canonical-per-vertex.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.h', s => {
    s = s.replace('   bool active;\n', `   bool active;
   /* Independent non-LTO stage ABI. A producer request needs only this flag;
    * VS lowering derives its complete layout without a fragment map. */
   bool canonical_heap;
`);
    s = s.replace('bool kk_nir_lower_fs_per_vertex_inputs(nir_shader *fs, nir_shader *prev,', `/* prev == NULL selects the canonical ABI when force or interface pressure
 * requires emulation. Linked/LTO callers keep passing their previous NIR.
 * The caller initializes map to zero; a false return leaves it inactive. */
bool kk_nir_lower_fs_per_vertex_inputs(nir_shader *fs, nir_shader *prev,`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_nir_lower_per_vertex.c', s => {
    s = s.replaceAll('uint64_t fixed_outputs = prev->info.outputs_written & ~(generic | VARYING_BIT_POS);',
      'uint64_t fixed_outputs = (prev ? prev->info.outputs_written : 0) &\n      ~(generic | VARYING_BIT_POS);');
    s = s.replaceAll('prev->info.clip_distance_array_size',
      '(prev ? prev->info.clip_distance_array_size : 0)');
    const loopStart = s.indexOf('   nir_foreach_block(block, nir_shader_get_entrypoint(prev)) {');
    const loopEnd = s.indexOf('   unsigned components =', loopStart);
    if (loopStart < 0 || loopEnd < 0) throw Error('Missing producer component-count span');
    const loop = s.slice(loopStart, loopEnd);
    s = s.replace(loop, '   if (prev) {\n' +
      loop.split('\n').map(line => line ? '   ' + line : line).join('\n') + '   }\n');
    s = s.replace('static nir_def *\nload_per_vertex_value(',
      '#include "kk_canonical_per_vertex.h"\n\nstatic nir_def *\nload_per_vertex_value(');
    s = s.replace(`   unsigned count = util_bitcount64(map->locations);
   unsigned index = util_bitcount64(map->locations & BITFIELD64_MASK(location));`,
`   unsigned count = map->canonical_heap ? KK_CANONICAL_VARYING_COUNT
                                        : util_bitcount64(map->locations);
   unsigned index = map->canonical_heap ? location - VARYING_SLOT_VAR0
      : util_bitcount64(map->locations & BITFIELD64_MASK(location));`);
    const start = s.indexOf('   nir_shader_gather_info(prev, nir_shader_get_entrypoint(prev));');
    const end = s.indexOf('   nir_builder b = nir_builder_create(impl);', start);
    if (start < 0 || end < 0) throw Error('Missing regular FS interface allocation span');
    const regular = s.slice(start, end);
    s = s.replace(regular, `   if (!prev) {
      kk_canonical_fs_map(fs, map);
   } else {
${regular.split('\n').map(line => line ? '   ' + line : line).join('\n')}   }

`);
    s = s.replace(`   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   if (!map || !map->active)
      return nir_no_progress(impl);

   nir_function *corner_function`,
`   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   struct kk_per_vertex_map canonical;
   if (map && map->canonical_heap) {
      nir_shader_gather_info(nir, impl);
      kk_canonical_map_init(&canonical);
      canonical.locations = nir->info.outputs_written & kk_canonical_generic_mask();
      map = &canonical;
   }
   if (!map || !map->active)
      return nir_no_progress(impl);

   nir_function *corner_function`);
    s = s.replace('      unsigned size = 3u * util_bitcount64(map->locations) * 16u;',
`      unsigned count = map->canonical_heap ? KK_CANONICAL_VARYING_COUNT
                                           : util_bitcount64(map->locations);
      unsigned size = 3u * count * 16u;`);
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_barycentric_shader.h', s => {
    s = s.replace('   VkPrimitiveTopology topology = state->ia->primitive_topology;',
      '   if (!state || !state->ia)\n      return true;\n   VkPrimitiveTopology topology = state->ia->primitive_topology;');
    s = s.replace('   return !state->rs ||', '   return !state || !state->rs ||');
    return s;
  });
}
