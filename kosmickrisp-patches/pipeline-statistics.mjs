import fs from 'node:fs';
import path from 'node:path';

/** Pipeline statistics use the pinned Honeykrisp/Poly counting model. */
export const id = 'pipeline-statistics';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-statistics.cl',
  'kosmickrisp-patches/kosmickrisp-statistics-command.h',
  'kosmickrisp-patches/kosmickrisp-statistics-nir.h',
];
export function apply({base, edits}) {
  function edit(name, apply) {
    const file = 'src/kosmickrisp/' + name;
    const before = edits.get(file) ?? fs.readFileSync(path.join(base, file), 'utf8');
    function replace(text, from, to) {
      if (!text.includes(from)) throw new Error(`${file}: missing statistics anchor ${from.slice(0, 80)}`);
      return text.replace(from, to);
    }
    edits.set(file, apply(before, replace));
  }

  edit('vulkan/kk_cmd_buffer.h', (s, replace) => replace(s,
    '      struct kk_compute_state cs;',
    `      struct kk_compute_state cs;
      uint64_t compute_statistic;
      uint64_t pipeline_statistics;
      VkQueryPipelineStatisticFlags pipeline_statistics_flags;`));

  edit('vulkan/kk_cmd_buffer.h', (s, replace) => replace(s,
    '         float blend_constant[4];',
    '         uint64_t fragment_statistic;\n         float blend_constant[4];'));

  edit('vulkan/kk_query_pool.c', (s, replace) => {
    s = replace(s, '   case VK_QUERY_TYPE_OCCLUSION:',
      `   case VK_QUERY_TYPE_PIPELINE_STATISTICS:
      return util_bitcount(pool->vk.pipeline_statistics);
   case VK_QUERY_TYPE_OCCLUSION:`);
    s = replace(s, '   pool =\n      vk_query_pool_create',
      `   /* Core statistics come from actual draw data, Poly stages, and
    * fragment execution. Additional statistic families are not exposed. */
   if (pCreateInfo->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS &&
       (pCreateInfo->pipelineStatistics == 0 ||
        (pCreateInfo->pipelineStatistics &
         ~0x7ffu)))
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);

   pool =
      vk_query_pool_create`);
    s = replace(s, '      index_size = sizeof(uint16_t) * pool->vk.query_count;',
      `      if (kk_pool_is_oq(pool))
         index_size = sizeof(uint16_t) * pool->vk.query_count;`);
    s = replace(s, '   uint32_t availability_size = 0;', '   uint64_t availability_size = 0;');
    s = replace(s, '   uint32_t index_size = 0;', '   uint64_t index_size = 0;');
    s = replace(s, 'pool->vk.query_count * sizeof(uint32_t)', '(uint64_t)pool->vk.query_count * sizeof(uint32_t)');
    s = replace(s, '   pool->index_start = align(availability_size, sizeof(struct kk_query_report));\n   uint32_t bo_size =',
      `   uint64_t index_start = align(availability_size, sizeof(struct kk_query_report));
   uint64_t bo_size =`);
    s = replace(s, '      align(pool->index_start + index_size, sizeof(struct kk_query_report));',
      `      align(index_start + index_size, sizeof(struct kk_query_report));
   if (bo_size > UINT32_MAX) {
      kk_DestroyQueryPool(device, kk_query_pool_to_handle(pool), pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   pool->index_start = index_start;`);
    s = replace(s, '      bo_size += pool->query_stride * pool->vk.query_count;',
      '      bo_size += (uint64_t)pool->query_stride * pool->vk.query_count;');
    s = replace(s, '   result = kk_alloc_bo(dev, &dev->vk.base, bo_size, 8, &pool->bo);',
      `   if (bo_size > UINT32_MAX) {
      kk_DestroyQueryPool(device, kk_query_pool_to_handle(pool), pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   result = kk_alloc_bo(dev, &dev->vk.base, bo_size, 8, &pool->bo);`);
    s = replace(s, '   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {',
      `   if (pool->vk.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
      uint64_t addr = kk_query_report_addr(kk_cmd_buffer_device(cmd), pool, query);
      unsigned mask = pool->vk.pipeline_statistics;
      cmd->state.pipeline_statistics = addr;
      cmd->state.pipeline_statistics_flags = mask;
      cmd->state.compute_statistic = (mask & VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT) ?
         addr + util_bitcount(mask & (VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT - 1)) * sizeof(uint64_t) : 0;
      cmd->state.gfx.descriptors.root.draw.fragment_statistic =
         (mask & VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT) ?
         addr + util_bitcount(mask & (VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT - 1)) * sizeof(uint64_t) : 0;
      cmd->state.gfx.descriptors.root_dirty = true;
      return;
   }
   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {`);
    // The first occurrence belongs to begin-query. Match the distinct end body.
    s = replace(s,
      `   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {
      cmd->state.gfx.xfb_query[index] = 0;`,
      `   if (pool->vk.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS) {
      cmd->state.compute_statistic = 0;
      cmd->state.pipeline_statistics_flags = 0;
      cmd->state.gfx.descriptors.root.draw.fragment_statistic = 0;
      cmd->state.gfx.descriptors.root_dirty = true;
   } else if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {
      cmd->state.gfx.xfb_query[index] = 0;`);
    return s;
  });

  edit('vulkan/kk_cmd_dispatch.c', (s, replace) => {
    s = replace(s, 'static void\nkk_predicate_compute(',
      `static void
kk_count_compute_invocations(struct kk_cmd_buffer *cmd, uint64_t indirect,
                             struct mtl_size groups, struct mtl_size local)
{
   if (!cmd->state.compute_statistic)
      return;

   libkk_count_compute(cmd, kk_grid_1d(1), false,
                       cmd->state.compute_statistic, indirect,
                       groups.x, groups.y, groups.z, local.x, local.y, local.z);
}

static void
kk_predicate_compute(`);
    s = replace(s, '   if (groupCountX * groupCountY * groupCountZ == 0)',
      '   if (groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)');
    s = replace(s, '      kk_predicate_compute(cmd, patched.gpu, patched.gpu);',
      `      kk_predicate_compute(cmd, patched.gpu, patched.gpu);
      kk_count_compute_invocations(cmd, patched.gpu, (struct mtl_size){0}, local_size);`);
    s = replace(s, '      kk_predicate_compute(cmd, patched.gpu, pInfo->addressRange.address);',
      `      kk_predicate_compute(cmd, patched.gpu, pInfo->addressRange.address);
      kk_count_compute_invocations(cmd, patched.gpu, (struct mtl_size){0}, local_size);`);
    s = replace(s, '      struct mtl_size grid_size = {',
      `      kk_count_compute_invocations(cmd, 0,
         (struct mtl_size){groupCountX, groupCountY, groupCountZ}, local_size);
      struct mtl_size grid_size = {`);
    s = replace(s,
      `   } else {
      kk_flush_compute_state(cmd);
      mtl_dispatch_threadgroups_with_indirect_buffer(`,
      `   } else {
      kk_count_compute_invocations(cmd, pInfo->addressRange.address,
                                    (struct mtl_size){0}, local_size);
      kk_flush_compute_state(cmd);
      mtl_dispatch_threadgroups_with_indirect_buffer(`);
    return s;
  });

  edit('libkk/kk_query.cl', s => s + '\n' + fs.readFileSync(
    new URL('./kosmickrisp-statistics.cl', import.meta.url), 'utf8'));

  edit('vulkan/kk_shader.h', (s, replace) => replace(s,
    '   bool has_geometry;', '   bool has_geometry;\n   bool api_geometry;'));

  edit('vulkan/kk_shader.c', (s, replace) => {
    s = replace(s, '#include "poly/geometry.h"', '#include "poly/geometry.h"\n#include "kk_statistics_nir.h"');
    s = replace(s, '      kk_lower_nir(dev, nir, emulated_stage, info->robustness,',
      `      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         NIR_PASS(_, nir, kk_nir_statistics_fs);
      kk_lower_nir(dev, nir, emulated_stage, info->robustness,`);
    s = replace(s, '      gather_graphics_pipeline_create_info(state, shaders, total_shaders);',
      `      gather_graphics_pipeline_create_info(state, shaders, total_shaders);
      shaders[0]->info.api_geometry = explicit_geometry;`);
    return s;
  });

  edit('vulkan/kk_nir_lower_descriptors.c', (s, replace) => replace(s,
    `      unsigned offset = kk_per_draw_offset(geometry_statistics) +`,
    `      if (b->shader->info.stage == MESA_SHADER_FRAGMENT &&
          nir_intrinsic_base(intrin) == PIPE_STAT_QUERY_PS_INVOCATIONS) {
         nir_def_replace(&intrin->def, load_root(b, 1, 64,
            nir_imm_int(b, offsetof(struct kk_root_descriptor_table, draw.fragment_statistic)), 8));
         return true;
      }
      unsigned offset = kk_per_draw_offset(geometry_statistics) +`));

  edit('vulkan/kk_cmd_draw.c', (s, replace) => {
    s = replace(s, 'static void\nkk_upload_tess_params(',
      `static uint64_t kk_pipeline_stat_addr(const struct kk_cmd_buffer *, VkQueryPipelineStatisticFlagBits);

static void
kk_upload_tess_params(`);
    s = replace(s, '      .statistic = 0u,',
      '      .statistic = kk_pool_upload(cmd, (uint64_t[1]){0}, sizeof(uint64_t), 8).gpu,');
    s = replace(s, 'kk_pool_upload(cmd, (uint64_t[16]){0}, 128, 8)',
      'kk_pool_upload(cmd, (uint64_t[32]){0}, 256, 8)');
    s = replace(s, '      gfx->per_draw_data.geometry_statistics[i] = scratch + 104;',
      '      gfx->per_draw_data.geometry_statistics[i] = scratch + 128 + i * sizeof(uint32_t);');
    s = replace(s, '   libkk_tessellate(cmd, grid_tess, true, info.mode, POLY_TESS_MODE_WITH_COUNTS,\n                    state);',
      `   libkk_tessellate(cmd, grid_tess, true, info.mode, POLY_TESS_MODE_WITH_COUNTS,
                    state);

   bool clip = !kk_geometry_shader(cmd);
   uint64_t tcs_stat = kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT);
   uint64_t tes_stat = kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT);
   uint64_t ci = clip ? kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT) : 0;
   uint64_t cp = clip ? kk_pipeline_stat_addr(cmd, VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT) : 0;
   if (tcs_stat || tes_stat || ci || cp)
      libkk_accumulate_tess_statistics(cmd, kk_grid_1d(1), true, state,
         tcs_stat, tes_stat, ci, cp, MAX2(1, util_bitcount(gfx->render.view_mask)));`);
    s = replace(s, '   bool queries = false;',
      `   bool queries = cmd->state.pipeline_statistics_flags &
      (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT);`);
    s = replace(s, 'static void\nkk_draw(struct kk_cmd_buffer *cmd,',
      '#include "kk_statistics_command.h"\n\nstatic void\nkk_draw(struct kk_cmd_buffer *cmd,');
    s = replace(s, '   /* Unroll geometry. Skip draw if we fail. */',
      '   kk_update_ia_statistics(cmd, data);\n\n   /* Unroll geometry. Skip draw if we fail. */');
    s = replace(s,
      `      if (geom)
         draw_data = kk_launch_gs_prerast(cmd, data, draw_data, gs_restart);`,
      `      if (geom) {
         draw_data = kk_launch_gs_prerast(cmd, data, draw_data, gs_restart);
         kk_finish_geometry_statistics(cmd);
      }`);
    return s;
  });

  for (const [file, owned] of [
    ['vulkan/kk_statistics_command.h', 'kosmickrisp-statistics-command.h'],
    ['vulkan/kk_statistics_nir.h', 'kosmickrisp-statistics-nir.h'],
  ]) edits.set('src/kosmickrisp/' + file, fs.readFileSync(new URL('./' + owned, import.meta.url), 'utf8'));

  edit('vulkan/kk_polygon_command.h', (s, replace) => replace(s,
    '   bool queries = false;',
    `   bool queries = cmd->state.pipeline_statistics_flags &
      (VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
       VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT);`));

  edit('vulkan/kk_physical_device.c', (s, replace) => replace(s,
    '      .occlusionQueryPrecise = true,',
    '      .occlusionQueryPrecise = true,\n      .pipelineStatisticsQuery = true,'));
}
