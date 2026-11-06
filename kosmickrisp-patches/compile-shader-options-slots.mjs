// SPDX-License-Identifier: MIT
import fs from 'node:fs';

export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/libkk/kk_argument_table.h',
    fs.readFileSync(new URL('./kosmickrisp-argument-table.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/vulkan/kk_compile_options.h', s => s.replace(
    '#include <stdbool.h>',
    '#include <stdbool.h>\n#include "kosmickrisp/libkk/kk_argument_table.h"'));
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => s.replace(
    '#include "nir.h"', '#include "nir.h"\n#include "kosmickrisp/libkk/kk_argument_table.h"'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s
    .replace(/mtl_set_max_buffer_binding_count\(desc, 3u(?: \+ \(dev->interlock_guard != NULL\))?\);/,
      `mtl_set_max_buffer_binding_count(desc,
         kk_argument_table_buffer_count((struct kk_argument_table_usage){
            .interlock_guard = dev->interlock_guard != NULL,
         }));`)
    .replace('mtl_set_address(cmd->argument_table, dev->samplers.table.bo->gpu, 1u);',
      'mtl_set_address(cmd->argument_table, dev->samplers.table.bo->gpu, KK_ARGUMENT_SAMPLERS);')
    .replace('mtl_set_address(cmd->argument_table, addr, 0u);',
      'mtl_set_address(cmd->argument_table, addr, KK_ARGUMENT_ROOT);')
    .replace('mtl_set_address(cmd->argument_table, data_gpu.gpu, 0u);',
      'mtl_set_address(cmd->argument_table, data_gpu.gpu, KK_ARGUMENT_ROOT);')
    .replace('mtl_set_address(cmd->argument_table, cmd->state.root_addr, 0u);',
      'mtl_set_address(cmd->argument_table, cmd->state.root_addr, KK_ARGUMENT_ROOT);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    'mtl_set_address(cmd->argument_table, shader_data_gpu.gpu, 2u);',
    'mtl_set_address(cmd->argument_table, shader_data_gpu.gpu, KK_ARGUMENT_PER_DRAW);'));
  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replaceAll(
    '.binding = 0', '.binding = KK_ARGUMENT_ROOT'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('P_IND(ctx, "constant Buffer &buf0 [[buffer(0)]],\\n");',
      'P_IND(ctx, "constant Buffer &buf0 [[buffer(%u)]],\\n", KK_ARGUMENT_ROOT);')
    .replace('P_IND(ctx, "constant SamplerTable &sampler_table [[buffer(1)]]");',
      'P_IND(ctx, "constant SamplerTable &sampler_table [[buffer(%u)]]", KK_ARGUMENT_SAMPLERS);')
    .replace('P_IND(ctx, "constant Buffer &per_draw [[buffer(2)]]\\n");',
      'P_IND(ctx, "constant Buffer &per_draw [[buffer(%u)]]\\n", KK_ARGUMENT_PER_DRAW);'));
}
