// SPDX-License-Identifier: MIT
/** Preserve fragment helper semantics and ordinary inactive scratch sizes. */
export const id = 'pipeline-statistics-compatibility';

export function apply({edits}) {
  function edit(file, apply) {
    const before = edits.get(file);
    if (before === undefined) throw new Error(`${file}: missing pipeline-statistics dependency`);
    function replace(text, from, to) {
      if (!text.includes(from)) throw new Error(`${file}: missing statistics correction anchor ${from.slice(0, 80)}`);
      return text.replace(from, to);
    }
    edits.set(file, apply(before, replace));
  }

  edit('src/kosmickrisp/vulkan/kk_shader.c', (s, replace) => {
    s = replace(s, `   if (feats->minLod)
      key |= BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD);`,
      `   if (feats->minLod)
      key |= BITFIELD_BIT(KK_FEAT_IMAGE_VIEW_MIN_LOD);
   if (feats->pipelineStatisticsQuery)
      key |= BITFIELD_BIT(KK_FEAT_PIPELINE_STATISTICS);`);
    s = replace(s, `      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         NIR_PASS(_, nir, kk_nir_statistics_fs);
      kk_lower_nir`, '      kk_lower_nir');
    return replace(s, '                   info->set_layout_count, info->set_layouts, state, features);',
      `                   info->set_layout_count, info->set_layouts, state, features);
      if (nir->info.stage == MESA_SHADER_FRAGMENT &&
          (features & BITFIELD_BIT(KK_FEAT_PIPELINE_STATISTICS)))
         NIR_PASS(_, nir, kk_nir_statistics_fs);`);
  });

  edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', (s, replace) => replace(s,
    `      if (b->shader->info.stage == MESA_SHADER_FRAGMENT &&
          nir_intrinsic_base(intrin) == PIPE_STAT_QUERY_PS_INVOCATIONS) {
         nir_def_replace(&intrin->def, load_root(b, 1, 64,
            nir_imm_int(b, offsetof(struct kk_root_descriptor_table, draw.fragment_statistic)), 8));
         return true;
      }
`, ''));

  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', (s, replace) => {
    s = replace(s, '      .statistic = kk_pool_upload(cmd, (uint64_t[1]){0}, sizeof(uint64_t), 8).gpu,',
      `      .statistic = (cmd->state.pipeline_statistics_flags &
         VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT) ?
         kk_pool_upload(cmd, (uint64_t[1]){0}, sizeof(uint64_t), 8).gpu : 0u,`);
    s = replace(s, '   uint64_t scratch = kk_pool_upload(cmd, (uint64_t[32]){0}, 256, 8).gpu;',
      `   bool statistics = cmd->state.pipeline_statistics_flags &
      (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT);
   static const uint64_t zeros[32] = {0};
   uint64_t scratch = kk_pool_upload(cmd, zeros, statistics ? 256 : 128, 8).gpu;`);
    return replace(s, '      gfx->per_draw_data.geometry_statistics[i] = scratch + 128 + i * sizeof(uint32_t);',
      `      gfx->per_draw_data.geometry_statistics[i] = statistics ?
         scratch + 128 + i * sizeof(uint32_t) : 0;`);
  });

  // Null gates apply only to pipeline statistics; XFB uses its original writes.
  edit('src/poly/cl/geometry.cl', (s, replace) => {
    for (const [counter, value] of [
      ['gs_invocations', 'unrolled_in_prims * invocations'],
      ['gs_primitives', 'emitted_prims'],
      ['c_primitives', 'emitted_prims'],
      ['c_invocations', 'emitted_prims'],
    ]) s = replace(s, `   *${counter} += ${value};`, `   if (${counter})\n      *${counter} += ${value};`);
    return s;
  });
}
