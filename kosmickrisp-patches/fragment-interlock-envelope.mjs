// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import {apply as applyInterlockSparseSubmit, sourceFiles as sparseSubmitFiles} from './interlock-sparse-submit.mjs';
import {requireHostDeferred, placeInterlockDeviceDrain, placeInterlockQueueDrain} from './interlock-shared-defer-hooks.mjs';
import {apply as applySourceAssembly, sourceFiles as sourceAssemblyFiles} from './interlock-source-assembly.mjs';
import {applyInterlockLayers, sourceFiles as layerFiles} from './interlock-layers.mjs';
export const id = 'fragment-interlock-envelope';
export const sourceFiles = [
  ...sparseSubmitFiles,
  'kosmickrisp-patches/interlock-shared-defer-hooks.mjs',
  ...sourceAssemblyFiles,
  ...layerFiles,
  'kosmickrisp-patches/kosmickrisp-interlock-select.h',
  'kosmickrisp-patches/kosmickrisp-interlock-envelope.h',
  'kosmickrisp-patches/kosmickrisp-interlock-program-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-inputs-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-tests-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-render-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-framebuffer-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-replay-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-load-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-shader.h',
  'kosmickrisp-patches/kosmickrisp-interlock-shader-data.h',
  'kosmickrisp-patches/kosmickrisp-interlock-pipeline.c',
  'kosmickrisp-patches/kosmickrisp-interlock-draw.h',
  'kosmickrisp-patches/kosmickrisp-interlock-draw.c',
  'kosmickrisp-patches/kosmickrisp-interlock-heap.h',
  'kosmickrisp-patches/kosmickrisp-interlock-heap.c',
  'kosmickrisp-patches/kosmickrisp-interlock-quad-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-capture-nir.h',
  'kosmickrisp-patches/kosmickrisp-interlock-submit.h',
  'kosmickrisp-patches/kosmickrisp-interlock-submit.c',
  'kosmickrisp-patches/kosmickrisp-interlock-continuation.h',
  'kosmickrisp-patches/kosmickrisp-interlock-continuation.m',
  'kosmickrisp-patches/kosmickrisp-interlock-continuation-stub.c',
];
export function apply({edit, edits}) {
  requireHostDeferred({edits});
  const owned = name => fs.readFileSync(new URL('./' + name, import.meta.url), 'utf8');
  const replace = (s, from, to) => {
    if (!s.includes(from)) throw Error('Missing interlock envelope anchor: ' + from.slice(0, 100));
    return s.replace(from, to);
  };
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => replace(s,
    '      supported_features.fragmentShaderSampleInterlock = true;',
    '      supported_features.fragmentShaderSampleInterlock = true;\n      supported_features.fragmentShaderPixelInterlock = true;'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => replace(s,
    `         if (BITSET_TEST(ctx->shader->info.system_values_read,
                         SYSTEM_VALUE_SAMPLE_MASK_IN))`,
    `         if (BITSET_TEST(ctx->shader->info.system_values_read,
                         SYSTEM_VALUE_SAMPLE_MASK_IN) &&
             (!(ctx->shader->info.fs.pixel_interlock_ordered ||
                ctx->shader->info.fs.pixel_interlock_unordered) ||
              ctx->shader->info.fs.uses_sample_shading))`));
  edit('src/kosmickrisp/compiler/nir_to_msl.h', s => replace(s,
    '   const struct vk_color_attachment_location_state *cal,\n   const struct vk_render_pass_state *rp);',
    '   const struct vk_color_attachment_location_state *cal,\n   const struct vk_render_pass_state *rp, bool interlock_shadow_samples);'));
  edit('src/kosmickrisp/compiler/msl_nir_lower_input_attachments.c', s => replace(replace(replace(s,
    '   const struct vk_render_pass_state *rp;\n};',
    '   const struct vk_render_pass_state *rp;\n   bool interlock_shadow_samples;\n};'),
    '   const struct vk_render_pass_state *rp)\n{',
    '   const struct vk_render_pass_state *rp, bool interlock_shadow_samples)\n{'),
    '         .rp = rp,', '         .rp = rp,\n         .interlock_shadow_samples = interlock_shadow_samples,'));
  edit('src/kosmickrisp/compiler/msl_attachment_samples.h', s => replace(replace(s,
    `      nir_def *current = nir_load_output(b, intr->def.num_components,
         intr->def.bit_size, nir_imm_int(b, 0),
         .dest_type = nir_intrinsic_dest_type(intr), .io_semantics = semantics);`,
    `      nir_def *current = state->interlock_shadow_samples ?
         nir_load_tile_image(b, intr->def.num_components, intr->def.bit_size,
            nir_imm_int(b, 0), multisample ? intr->src[2].ssa : nir_imm_int(b, 0),
            .dest_type = nir_intrinsic_dest_type(intr), .io_semantics = semantics,
            .access = ACCESS_COHERENT) :
         nir_load_output(b, intr->def.num_components,
            intr->def.bit_size, nir_imm_int(b, 0),
            .dest_type = nir_intrinsic_dest_type(intr), .io_semantics = semantics);`),
    '      if (multisample) {', '      if (multisample && !state->interlock_shadow_samples) {'));
  for (const [target, input] of [
    ['vulkan/kk_interlock_select.h', 'kosmickrisp-interlock-select.h'],
    ['vulkan/kk_interlock_envelope.h', 'kosmickrisp-interlock-envelope.h'],
    ['vulkan/kk_interlock_program_nir.h', 'kosmickrisp-interlock-program-nir.h'],
    ['vulkan/kk_interlock_inputs_nir.h', 'kosmickrisp-interlock-inputs-nir.h'],
    ['vulkan/kk_interlock_tests_nir.h', 'kosmickrisp-interlock-tests-nir.h'],
    ['vulkan/kk_interlock_render_nir.h', 'kosmickrisp-interlock-render-nir.h'],
    ['vulkan/kk_interlock_framebuffer_nir.h', 'kosmickrisp-interlock-framebuffer-nir.h'],
    ['vulkan/kk_interlock_replay_nir.h', 'kosmickrisp-interlock-replay-nir.h'],
    ['vulkan/kk_interlock_load_nir.h', 'kosmickrisp-interlock-load-nir.h'],
    ['vulkan/kk_interlock_shader.h', 'kosmickrisp-interlock-shader.h'],
    ['vulkan/kk_interlock_shader_data.h', 'kosmickrisp-interlock-shader-data.h'],
    ['vulkan/kk_interlock_pipeline.c', 'kosmickrisp-interlock-pipeline.c'],
    ['vulkan/kk_interlock_draw.h', 'kosmickrisp-interlock-draw.h'],
    ['vulkan/kk_interlock_draw.c', 'kosmickrisp-interlock-draw.c'],
    ['vulkan/kk_interlock_heap.h', 'kosmickrisp-interlock-heap.h'],
    ['vulkan/kk_interlock_heap.c', 'kosmickrisp-interlock-heap.c'],
    ['vulkan/kk_interlock_quad_nir.h', 'kosmickrisp-interlock-quad-nir.h'],
    ['vulkan/kk_interlock_capture_nir.h', 'kosmickrisp-interlock-capture-nir.h'],
    ['vulkan/kk_interlock_submit.h', 'kosmickrisp-interlock-submit.h'],
    ['vulkan/kk_interlock_submit.c', 'kosmickrisp-interlock-submit.c'],
    ['bridge/mtl_interlock_continuation.h', 'kosmickrisp-interlock-continuation.h'],
    ['bridge/mtl_interlock_continuation.m', 'kosmickrisp-interlock-continuation.m'],
    ['bridge/stubs/mtl_interlock_continuation.c', 'kosmickrisp-interlock-continuation-stub.c'],
  ]) edits.set('src/kosmickrisp/' + target, owned(input));
  edit('src/kosmickrisp/vulkan/meson.build', s => replace(s,
    "  'kk_queue.c',", "  'kk_queue.c',\n  'kk_interlock_submit.c',\n  'kk_interlock_heap.c',\n  'kk_interlock_pipeline.c',\n  'kk_interlock_draw.c',"));
  edit('src/kosmickrisp/bridge/meson.build', s => replace(replace(s,
    "    'mtl_command_queue.m',", "    'mtl_command_queue.m',\n    'mtl_interlock_continuation.m',"),
    "    'stubs/mtl_command_queue.c',", "    'stubs/mtl_command_queue.c',\n    'stubs/mtl_interlock_continuation.c',"));
  edit('src/kosmickrisp/vulkan/kk_device.h', s => replace(s,
    '   struct kk_bo *interlock_guard;',
    '   struct kk_bo *interlock_guard;\n   struct kk_interlock_registry *interlock_registry;'));
  edit('src/kosmickrisp/vulkan/kk_queue.h', s => replace(s,
    '   struct mtl_command_queue *mtl_handle;',
    '   struct mtl_command_queue *mtl_handle;\n   struct kk_interlock_queue *interlock_submission;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => replace(s,
    '   struct vk_command_buffer vk;',
    '   struct vk_command_buffer vk;\n   struct kk_interlock_segment *interlock_segments;\n   struct kk_interlock_heap *interlock_heap;\n   bool interlock_heap_active;\n   bool interlock_heap_used;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => replace(s,
    '   uint64_t polygon_capture;',
    '   uint64_t polygon_capture;\n   struct kk_ptr interlock_capture;\n   uint32_t interlock_source_view;\n   uint32_t interlock_source_view_ordinal;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => {
    s = replace(s, '#include "kk_cmd_buffer.h"', '#include "kk_cmd_buffer.h"\n#include "kk_interlock_submit.h"\n#include "kk_interlock_heap.h"\n#include "kk_interlock_draw.h"');
    // Move the existing constructor intact: it already consumes the shared
    // slot/capacity API on integrated heads. There is no second calculation.
    const argumentBlock = /   \{\n      mtl_argument_table_descriptor \*desc = mtl_new_argument_table_descriptor\(\);[\s\S]*?      mtl_release\(desc\);\n   \}/;
    const match = s.match(argumentBlock);
    if (!match) throw Error('Missing ordinary argument-table constructor');
    const body = match[0].slice(5, -5).replaceAll('cmd->argument_table', 'table');
    s = s.replace(argumentBlock,
      '   cmd->argument_table = kk_new_command_argument_table(dev);');
    const hook = 'static VkResult\nkk_create_cmd_buffer(';
    s = replace(s, hook, `mtl_argument_table *
kk_new_command_argument_table(struct kk_device *dev)
{
   mtl_argument_table *table;
${body}
   return table;
}

${hook}`);
    return replace(s, '   kk_cmd_release_dynamic_ds_state(cmd);',
      '   kk_interlock_segments_finish(cmd);\n   kk_interlock_heap_finish(cmd);\n   kk_cmd_release_dynamic_ds_state(cmd);');
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
    s = replace(s, '#include "kk_cmd_buffer.h"',
      '#include "kk_cmd_buffer.h"\n#include "kk_interlock_heap.h"\n#include "kk_interlock_draw.h"\n#include "kk_interlock_envelope.h"');
    s = replace(s, '   size_t size = 128 * 1024 * 1024;',
      '   size_t size = KK_POLY_HEAP_SIZE;');
    const heapStart = s.indexOf('static uint64_t\nkk_heap(');
    if (heapStart < 0) throw Error('Missing Poly heap selection anchor');
    let tail = String(s).slice(heapStart);
    tail = replace(tail, '   if (!cmd->uses_heap) {',
      '   bool *uses_heap = cmd->interlock_heap_active ?\n      &cmd->interlock_heap_used : &cmd->uses_heap;\n   if (!*uses_heap) {');
    tail = replace(tail, '      cmd->uses_heap = true;', '      *uses_heap = true;');
    tail = replace(tail, '   util_call_once_data(&dev->heap_init_once, kk_init_heap, cmd);',
      '   kk_draw_heap(cmd);');
    tail = tail.replaceAll('dev->heap->', 'kk_draw_heap(cmd)->');
    s = String(s).slice(0, heapStart) + `static struct kk_bo *
kk_draw_heap(struct kk_cmd_buffer *cmd)
{
   if (cmd->interlock_heap_active)
      return kk_interlock_heap_bo(cmd->interlock_heap);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   util_call_once_data(&dev->heap_init_once, kk_init_heap, cmd);
   return dev->heap;
}

` + tail;
    const drawFailure = s.includes('\nlayered_bc_done:\n') ?
      'goto layered_bc_done;' : 'return;';
    s = replace(s, '   kk_flush_gfx_state(cmd);', `   if (vs->info.interlock_envelope || cmd->interlock_heap_active) {
      VkResult result = kk_interlock_heap_select(cmd,
         vs->info.interlock_envelope && dyn->ms.rasterization_samples > 1);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd->vk, result);
         ${drawFailure}
      }
   }
   kk_flush_gfx_state(cmd);`);
    s = replace(s, '   if (gfx->polygon_points) {\n      const struct poly_gs_capture_info *capture',
      '   if (gfx->polygon_points || cmd->interlock_heap_active) {\n      const struct poly_gs_capture_info *capture');
    s = replace(s,
      '      params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;',
      `      if (cmd->interlock_heap_active) {
         gfx->interlock_capture = kk_pool_alloc(cmd, sizeof(struct kk_interlock_envelope), 8);
         if (gfx->interlock_capture.cpu) {
            memset(gfx->interlock_capture.cpu, 0, sizeof(struct kk_interlock_envelope));
            struct kk_interlock_envelope *env = gfx->interlock_capture.cpu;
            env->geometry = point;
            env->view_mask = gfx->render.view_mask;
            env->source_view = gfx->interlock_source_view;
            env->source_view_ordinal = gfx->interlock_source_view_ordinal;
         }
         params.capture = gfx->polygon_capture = gfx->interlock_capture.gpu;
      } else {
         params.capture = gfx->polygon_capture = kk_pool_upload(cmd, &point, sizeof(point), 8).gpu;
      }`);
    s = replace(s, '   if (gfx->polygon_points)\n      return kk_launch_polygon_points',
      '   if (gfx->polygon_points || cmd->interlock_heap_active)\n      return kk_launch_polygon_points');
    s = replace(s, '      kk_poly_layer_dispatch_draw(cmd, draw_data);', `      if (cmd->interlock_heap_active) {
         VkResult result = kk_interlock_record_draw(cmd);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd->vk, result);
            ${drawFailure}
         }
      } else {
         kk_poly_layer_dispatch_draw(cmd, draw_data);
      }
      }`);
    s = replace(s, '   for (uint32_t i = 0; i < data->draw_count; i++) {\n      struct kk_draw_data draw_data',
      `   for (uint32_t i = 0; i < data->draw_count; i++) {
      uint32_t source_views = cmd->interlock_heap_active && gfx->render.view_mask ?
         gfx->render.view_mask : 1;
      unsigned source_ordinal = 0;
      while (source_views) {
         gfx->interlock_source_view = gfx->render.view_mask && cmd->interlock_heap_active ?
            ffs(source_views) - 1 : 0;
         gfx->interlock_source_view_ordinal = source_ordinal++;
         source_views &= source_views - 1;
      struct kk_draw_data draw_data`);
    s = replace(s, '      if (data->upload_mask)\n         kk_upload_per_draw_data(cmd, data->upload_mask, i + gfx->indirect_draw_id, &draw_data,',
      `      if (data->upload_mask || cmd->interlock_heap_active)
         kk_upload_per_draw_data(cmd, data->upload_mask |
            (cmd->interlock_heap_active ? BITFIELD_BIT(MESA_SHADER_GEOMETRY) : 0), i + gfx->indirect_draw_id, &draw_data,`);
    s = replace(s, 'tcs_stat, tes_stat, ci, cp, MAX2(1, util_bitcount(gfx->render.view_mask)));',
      'tcs_stat, tes_stat, ci, cp, cmd->interlock_heap_active ? 1 : MAX2(1, util_bitcount(gfx->render.view_mask)));');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_statistics_command.h', s => replace(s,
    '      MAX2(1, util_bitcount(cmd->state.gfx.render.view_mask)));',
    '      cmd->interlock_heap_active ? 1 : MAX2(1, util_bitcount(cmd->state.gfx.render.view_mask)));'));
  edit('src/kosmickrisp/vulkan/kk_device.c', s => {
    s = replace(s, '#include "kk_interlock_device.h"',
      '#include "kk_interlock_device.h"\n#include "kk_interlock_submit.h"');
    s = replace(s, '   simple_mtx_init(&dev->blit_queue_mutex, mtx_plain);',
      `   simple_mtx_init(&dev->blit_queue_mutex, mtx_plain);
   result = kk_interlock_submission_init(dev);
   if (result != VK_SUCCESS)
      goto fail_vab_memory;`);
    // The optional guard must also be released if the later atomic-lock
    // allocation fails during device construction.
    s = replace(s, 'fail_sampler_heap:',
      'fail_sampler_heap:\n   kk_interlock_device_finish(dev);');
    s = replace(s, 'fail_vab_memory:',
      'fail_vab_memory:\n   kk_interlock_submission_finish(dev);');
    s = placeInterlockDeviceDrain(s);
    s = replace(s, '   kk_release_compiler(dev);\n\n   vk_device_finish(&dev->vk);',
      '   kk_release_compiler(dev);\n   kk_interlock_submission_finish(dev);\n\n   vk_device_finish(&dev->vk);');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_queue.c', s => {
    s = replace(s, '#include "kk_sync.h"', '#include "kk_sync.h"\n#include "kk_interlock_submit.h"');
    s = replace(s, '         mtl_command_queue_error_to_string(data->error), data->error_message);',
      '         mtl_command_queue_error_to_string(data->error), data->error_message);\n      kk_interlock_submission_lost(dev);');
    s = replace(s, 'static void\nkk_queue_commit(', 'static VkResult\nkk_queue_commit(');
    s = replace(s, '   assert(kk_cmd_buffer_has_work(cmd));',
      `   assert(kk_cmd_buffer_has_work(cmd));
   if (cmd->interlock_segments)
      return kk_interlock_commit(queue, cmd, callback, user_data);`);
    s = replace(s, '      cmd->drawable = NULL;\n   }\n}\n\n#include "kk_sparse_queue.h"', '      cmd->drawable = NULL;\n   }\n   return VK_SUCCESS;\n}\n\n#include "kk_sparse_queue.h"');
    s = replace(s, '         if (result != VK_SUCCESS)\n            return result;',
      `         if (result != VK_SUCCESS)
            return result == VK_ERROR_DEVICE_LOST
               ? kk_interlock_submission_lost(dev) : result;`);
    s = placeInterlockQueueDrain(s);
    s = replace(s, '   queue->commits_in_flight = 0u;',
      '   queue->commits_in_flight = 0u;\n   queue->interlock_submission = NULL;');
    return s;
  });
  applyInterlockSparseSubmit({edit});
  edit('src/kosmickrisp/vulkan/kk_sync.h', s => replace(replace(s,
    '#include "vk_sync.h"', '#include "vk_sync.h"\n#include "util/list.h"'),
    '   mtl_shared_event *mtl_handle;',
    '   mtl_shared_event *mtl_handle;\n   struct list_head interlock_link;\n   bool interlock_registered;'));
  edit('src/kosmickrisp/vulkan/kk_sync.c', s => {
    s = replace(s, '#include "kk_device.h"', '#include "kk_device.h"\n#include "kk_interlock_submit.h"');
    s = replace(s, '   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);',
      `   timeline->mtl_handle = mtl_new_shared_event(dev->mtl_handle);
   if (!timeline->mtl_handle)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;`);
    s = replace(s, '   mtl_shared_event_set_signaled_value(timeline->mtl_handle, initial_value);',
      '   mtl_shared_event_set_signaled_value(timeline->mtl_handle, initial_value);\n   kk_interlock_sync_init(dev, timeline);');
    s = replace(s, '   mtl_release(timeline->mtl_handle);',
      '   kk_interlock_sync_finish(container_of(device, struct kk_device, vk), timeline);\n   mtl_release(timeline->mtl_handle);');
    s = replace(s, '   mtl_shared_event_set_signaled_value(timeline->mtl_handle, value);\n   kk_sync_mark_pending(timeline, value);\n   mtx_unlock(&timeline->publish_mutex);\n   return VK_SUCCESS;',
      '   mtl_shared_event_set_signaled_value(timeline->mtl_handle, value);\n   kk_sync_mark_pending(timeline, value);\n   mtx_unlock(&timeline->publish_mutex);\n   return kk_interlock_sync_result(device, VK_SUCCESS);');
    s = s.replaceAll('return vk_device_is_lost(device) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;',
      'return kk_interlock_sync_result(device, vk_device_is_lost(device) ? VK_ERROR_DEVICE_LOST : VK_SUCCESS);');
    return s;
  });
  edit('src/vulkan/runtime/vk_shader.h', s => replace(s,
    '   /** Hash a vk_graphics_state object and a vk_features object.',
    '   /** Require one compilation partition for all available graphics stages. */\n   bool (*require_graphics_stage_linking)(struct vk_device *device);\n\n   /** Hash a vk_graphics_state object and a vk_features object.'));
  edit('src/vulkan/runtime/vk_pipeline.c', s => replace(s,
    '   } else if (info->optimize) {',
    '   } else if (info->optimize ||\n              (device->shader_ops->require_graphics_stage_linking &&\n               device->shader_ops->require_graphics_stage_linking(device))) {'));
  edit('src/kosmickrisp/vulkan/kk_compile_options.h', s => replace(s,
    '   bool poly_layer_replay;',
    '   bool poly_layer_replay;\n   bool pixel_envelope;\n   bool pixel_sample_shading;'));
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => replace(replace(s,
    '   KK_FEAT_COUNT,', '   KK_FEAT_PIXEL_ENVELOPE,\n   KK_FEAT_COUNT,'),
    'struct kk_shader_info {', 'struct kk_shader_info {\n   bool interlock_envelope;').replace(
    '   struct vk_shader vk;',
    '   struct vk_shader vk;\n   struct kk_interlock_shader *interlock;\n   struct kk_interlock_pipeline *interlock_pipeline;'));
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = replace(s, '#include "kk_barycentric_shader.h"',
      '#include "kk_barycentric_shader.h"\n#include "kk_interlock_select.h"');
    s = replace(s, 'static void\nkk_shader_destroy(',
      'static void kk_interlock_shader_finish(struct kk_interlock_shader *);\nvoid kk_interlock_pipeline_finish(struct kk_interlock_pipeline *);\n\nstatic void\nkk_shader_destroy(');
    s = replace(s, '   struct kk_pipeline_handles *pipe = &shader->pipeline;',
      '   kk_interlock_pipeline_finish(shader->interlock_pipeline);\n   kk_interlock_shader_finish(shader->interlock);\n   struct kk_pipeline_handles *pipe = &shader->pipeline;');
    s = replace(s, '   /* Must be lowered before blending */\n   kk_lower_multisample_alpha(nir, state);',
      `   const bool pixel_envelope = dev->vk.enabled_features.fragmentShaderPixelInterlock &&
      kk_interlock_multisample_pipeline(state) && kk_interlock_pixel_shader(nir);
   /* Envelope output processing applies these shared passes per sample,
    * after the original pixel-frequency body has executed only once. */
   if (!pixel_envelope)
      kk_lower_multisample_alpha(nir, state);`);
    s = replace(s, '   if (state->cb)\n      kk_lower_fs_blend(nir, state);',
      '   if (!pixel_envelope && state->cb)\n      kk_lower_fs_blend(nir, state);');
    s = replace(s, '   if (kk_fs_has_sample_mask(state)) {',
      '   if (!pixel_envelope && kk_fs_has_sample_mask(state)) {');
    s = replace(s,
      '      msl_nir_lower_input_attachments(nir, state->ial, state->cal,\n                                      state->rp);',
      `      msl_nir_lower_input_attachments(nir, state->ial, state->cal,
         state->rp, dev->vk.enabled_features.fragmentShaderPixelInterlock &&
            kk_interlock_multisample_pipeline(state) && kk_interlock_pixel_shader(nir));`);
    s = replace(s,
      '       intrin->intrinsic != nir_intrinsic_load_output)',
      '       intrin->intrinsic != nir_intrinsic_load_output &&\n       intrin->intrinsic != nir_intrinsic_load_tile_image)');
    s = replace(s, '#include "kk_polygon_nir.h"',
      '#include "kk_polygon_nir.h"\n#include "kk_interlock_shader.h"');
    s = replace(s, '   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts,',
      `   if (pixel_envelope && stage == MESA_SHADER_FRAGMENT) {
      result = kk_interlock_compile_fragment(dev, shader, nir, prev_stage, state, robustness, pixel_sample_shading);
      if (result != VK_SUCCESS) {
         ralloc_free(nir);
         kk_shader_destroy(&dev->vk, shader, pAllocator);
         return result;
      }
   }
   if (pixel_envelope && stage == MESA_SHADER_FRAGMENT)
      nir_shader_intrinsics_pass(nir, kk_interlock_trim_native_output,
         nir_metadata_control_flow, (void *)state);
   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts,`);
    s = replace(s, '      kk_msl_copy(&vs->msl_data[stage], &s->msl_data[stage]);',
      `      kk_msl_copy(&vs->msl_data[stage], &s->msl_data[stage]);
      if (stage == MESA_SHADER_FRAGMENT && s->interlock) {
         kk_interlock_shader_finish(vs->interlock);
         vs->interlock = kk_interlock_shader_ref(s->interlock);
         info->interlock_envelope = true;
      }`);
    s = replace(s, '   return !blob->out_of_memory;',
      '   kk_interlock_shader_serialize(shader, blob);\n   return !blob->out_of_memory;');
    s = replace(s, '   if (info.stage == MESA_SHADER_COMPUTE) {\n      uint32_t local_size_threads',
      '   result = kk_interlock_shader_deserialize(shader, blob);\n   if (result != VK_SUCCESS) goto fail;\n   if (info.stage == MESA_SHADER_COMPUTE) {\n      uint32_t local_size_threads');
    s = replace(s, '   kk_feature_key key = 0;',
      '   kk_feature_key key = 0;\n   if (feats->fragmentShaderPixelInterlock)\n      key |= BITFIELD_BIT(KK_FEAT_PIXEL_ENVELOPE);');
    s = replace(s, '   const struct vk_pipeline_robustness_state *vertex_robustness = &rs_none;\n   for (uint32_t i = 0u; i < shader_count; ++i) {',
      `   const struct vk_pipeline_robustness_state *vertex_robustness = &rs_none;
   bool pixel_envelope = false;
   bool pixel_sample_shading = false;
   const bool pixel_candidate = enabled_features->fragmentShaderPixelInterlock &&
      shader_count && kk_interlock_multisample_pipeline(state) &&
      kk_interlock_pixel_shader(infos[shader_count - 1u].nir);
   for (uint32_t step = 0u; step < shader_count; ++step) {
      uint32_t i = pixel_candidate ? (step ? step - 1u : shader_count - 1u) : step;`);
    s = replace(s, '      /* XFB without an explicit GS gets a Poly passthrough after linking.',
      `      emulated_stage |= pixel_envelope &&
         (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL);
      /* XFB without an explicit GS gets a Poly passthrough after linking.`);
    s = replace(s, '      msl_preprocess_nir_workarounds(nir, pdev->settings.disabled_workarounds);',
      `      if (pixel_candidate && nir->info.stage == MESA_SHADER_FRAGMENT) {
         pixel_envelope = true;
         pixel_sample_shading = kk_interlock_sample_frequency(nir) ||
            (state->ms && state->ms->sample_shading_enable);
      }
      msl_preprocess_nir_workarounds(nir, pdev->settings.disabled_workarounds);`);
    s = replace(s, 'if (!explicit_geometry && (xfb || kk_polygon_possible(dev, state) || poly_layer_replay)) {',
      'if (!explicit_geometry && (xfb || pixel_envelope || kk_polygon_possible(dev, state) || poly_layer_replay)) {');
    s = replace(s, '      if (kk_polygon_possible(dev, state) || poly_layer_replay)\n         nir_shader_intrinsics_pass(builder.shader, kk_polygon_passthrough_primitive_id,',
      '      if (pixel_envelope || kk_polygon_possible(dev, state) || poly_layer_replay)\n         nir_shader_intrinsics_pass(builder.shader, kk_polygon_passthrough_primitive_id,');
    s = replace(s, '   const struct kk_per_vertex_map *per_vertex_map = opts->per_vertex_map;',
      '   const bool pixel_envelope = opts->pixel_envelope;\n   const bool pixel_sample_shading = opts->pixel_sample_shading;\n   const struct kk_per_vertex_map *per_vertex_map = opts->per_vertex_map;');
    s = replace(s, '   gather_shader_info(shader, nir, state);',
      `   gather_shader_info(shader, nir, state);
   shader->info.interlock_envelope = pixel_envelope;
   if (pixel_envelope && stage == MESA_SHADER_FRAGMENT)
      shader->info.fs.noperspective_outputs = kk_polygon_noperspective_outputs(nir);`);
    s = replace(s, `   if (stage == MESA_SHADER_FRAGMENT &&
       (kk_polygon_possible(dev, state) || opts->poly_layer_replay)) {`,
      `   if (stage == MESA_SHADER_FRAGMENT &&
       (pixel_envelope || kk_polygon_possible(dev, state) || opts->poly_layer_replay)) {`);
    s = replace(s, '      NIR_PASS(_, point_fs, kk_nir_lower_descriptors, false, robustness, 0, NULL);',
      `      if (pixel_envelope)
         nir_shader_intrinsics_pass(point_fs, kk_interlock_trim_native_output,
            nir_metadata_control_flow, (void *)state);
      NIR_PASS(_, point_fs, kk_nir_lower_descriptors, false, robustness, 0, NULL);`);
    s = s.replaceAll('if (kk_polygon_regular_possible(dev, state) || opts->poly_layer_replay)',
      'if (pixel_envelope || kk_polygon_regular_possible(dev, state) || opts->poly_layer_replay)');
    // Capture uses the existing Poly programs and cache storage for both
    // consumers. The application GS still runs exactly once.
    s = replace(s, '               polygon ? &capture : NULL, polygon ? &capture_xfb : NULL);',
      '               (polygon || pixel_envelope) ? &capture : NULL,\n               (polygon || pixel_envelope) ? &capture_xfb : NULL);');
    s = replace(s, '      if (polygon) {\n         shader->info.polygon_point = true;',
      '      if (polygon || pixel_envelope) {\n         shader->info.polygon_point = polygon;');
    s = replace(s, '      if (stage == MESA_SHADER_FRAGMENT && (info->polygon_point || info->poly_layer_replay))',
      '      if (stage == MESA_SHADER_FRAGMENT && (info->polygon_point || info->poly_layer_replay || info->interlock_envelope))');
    s = replace(s, '         if (info->polygon_point || info->poly_layer_replay) {',
      '         if (info->polygon_point || info->poly_layer_replay || info->interlock_envelope) {');
    s = replace(s, '   if (result != VK_SUCCESS || !(vs->info.polygon_point || vs->info.poly_layer_replay))',
      '   if (result != VK_SUCCESS || !(vs->info.polygon_point || vs->info.poly_layer_replay || vs->info.interlock_envelope))');
    s = replace(s, '   if (result == VK_SUCCESS && vs->info.conservative)\n      result = kk_compile_graphics_pipeline_native(device, vs, 2);',
      '   if (result == VK_SUCCESS && vs->info.conservative)\n      result = kk_compile_graphics_pipeline_native(device, vs, 2);\n   if (result == VK_SUCCESS && vs->interlock)\n      result = kk_interlock_pipeline_compile(device, vs);');
    s = replace(s, '   if (vs->info.polygon_point) {',
      '   if (vs->info.polygon_point || vs->info.interlock_envelope) {');
    s = replace(s, '   if (shader->info.polygon_point || shader->info.poly_layer_replay) {',
      '   if (shader->info.polygon_point || shader->info.poly_layer_replay || shader->info.interlock_envelope) {');
    s = replace(s, '   if (info.polygon_point || info.poly_layer_replay) {',
      '   if (info.polygon_point || info.poly_layer_replay || info.interlock_envelope) {');
    s = replace(s, '   const struct kk_compile_options compile_options = {',
      '   const struct kk_compile_options compile_options = {\n      .pixel_envelope = pixel_envelope,\n      .pixel_sample_shading = pixel_sample_shading,');
    s = replace(s, '   .get_nir_options = kk_get_nir_options,',
      '   .get_nir_options = kk_get_nir_options,\n   .require_graphics_stage_linking = kk_interlock_link_graphics,');
    return s;
  });
  applySourceAssembly({edit, edits});
  applyInterlockLayers({edit, edits});
}
