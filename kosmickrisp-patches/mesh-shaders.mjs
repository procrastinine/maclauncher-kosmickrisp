// SPDX-License-Identifier: MIT
import fs from 'node:fs';

/** Native mesh stages with bounded task-grid virtualization and retained raster output. */
export const id = 'mesh-shaders';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-mesh-types.h',
  'kosmickrisp-patches/kosmickrisp-mesh-dispatch.h',
  'kosmickrisp-patches/kosmickrisp-mesh-sliced.h',
  'kosmickrisp-patches/kosmickrisp-mesh-api.h',
  'kosmickrisp-patches/kosmickrisp-mesh-nir.h',
  'kosmickrisp-patches/kosmickrisp-mesh.cl',
  'kosmickrisp-patches/kosmickrisp-mesh-kernels.h',
  'kosmickrisp-patches/kosmickrisp-mesh-iomap.h',
  'kosmickrisp-patches/kosmickrisp-mesh-intrinsics.h',
  'kosmickrisp-patches/kosmickrisp-mesh-msl.h',
  'kosmickrisp-patches/kosmickrisp-mesh-retained-msl.h',
  'kosmickrisp-patches/kosmickrisp-mesh.metal',
  'kosmickrisp-patches/kosmickrisp-mesh-bridge.m',
  'kosmickrisp-patches/kosmickrisp-mesh-pipeline.h',
  'kosmickrisp-patches/kosmickrisp-mesh-command.h',
  'kosmickrisp-patches/kosmickrisp-canonical-layout.h',
  'kosmickrisp-patches/kosmickrisp-mesh-canonical.h',
  'kosmickrisp-patches/kosmickrisp-canonical-projected.h',
];

export function apply({edit, edits}) {
  const kk = (file, fn) => edit('src/kosmickrisp/' + file, fn);
  kk('vulkan/kk_shader.h', s => s
    .replace('msl_data[MESA_SHADER_STAGES]', 'msl_data[MESA_ALL_SHADER_STAGES]')
    .replace('   KK_FEAT_COUNT,', '   KK_FEAT_MESH_QUERIES,\n   KK_FEAT_COUNT,')
    .replace('struct kk_cmd_buffer;', `#include "kosmickrisp/libkk/kk_mesh_types.h"
#include "kk_mesh_pipeline_api.h"

enum kk_pipeline_kind {
   KK_PIPELINE_MAIN,
   KK_PIPELINE_POLYGON,
   KK_PIPELINE_CONSERVATIVE,
   KK_PIPELINE_MESH_VIRTUAL,
   KK_PIPELINE_MESH_SLICED,
   KK_PIPELINE_MESH_VIRTUAL_SLICED,
   KK_PIPELINE_MESH_CAPTURE,
   KK_PIPELINE_MESH_READER,
   KK_PIPELINE_KIND_COUNT,
};

struct kk_cmd_buffer;`)
    .replace('struct kk_shader_info {', `struct kk_mesh_shader_info {
   struct mtl_size local_size;
   struct mtl_size task_local_size;
   uint32_t payload_size;
   uint32_t task_payload_size;
   uint32_t task_grid_bound;
   enum mesa_prim primitive;
   bool has_task;
   bool queries;
   bool sliced;
   bool retained_capture, retained_reader;
   uint32_t max_vertices;
   uint32_t retained_strides[2];
   struct kk_mesh_retained_field retained_fields[KK_MESH_RETAINED_FIELD_COUNT];
   uint32_t view_mask;
};

struct kk_shader_info {`)
    .replace('   bool uses_per_draw_data;', '   bool uses_per_draw_data;\n   struct kk_mesh_shader_info mesh;')
    .replace('   struct kk_pipeline_handles pipeline;',
      '   struct kk_pipeline_handles pipeline;\n   struct kk_pipeline_handles *mesh_virtual;')
    .replace('   struct kk_shader *shader, unsigned polygon);',
      '   struct kk_shader *shader, unsigned kind);')
    .replace('   struct kk_pipeline_handles *mesh_virtual;',
      '   struct kk_pipeline_handles *mesh_virtual;\n   struct kk_pipeline_handles *mesh_sliced[2];\n   struct kk_pipeline_handles *mesh_retained[2];\n   struct kk_mesh_fragment_program *mesh_fragment;')
    .replace('extern const struct vk_device_shader_ops kk_device_shader_ops;', `VkResult kk_mesh_get_pipeline(struct kk_device *dev, struct kk_shader *shader,
   bool force_virtual, struct kk_pipeline_handles **out);

VkResult kk_mesh_get_sliced_pipeline(struct kk_device *dev, struct kk_shader *shader,
   bool virtual, struct kk_pipeline_handles **out);
void kk_mesh_release_pipeline(struct kk_device *dev, struct kk_shader *shader,
   struct kk_pipeline_handles *pipe, unsigned kind);

extern const struct vk_device_shader_ops kk_device_shader_ops;`));
  kk('vulkan/kk_shader.c', s => s
    .replace('      .has_base_workgroup_id = true,',
      '      .has_base_workgroup_id = nir->info.stage == MESA_SHADER_COMPUTE,')
    .replace('   return key;\n}', `   if (feats->meshShaderQueries || feats->pipelineStatisticsQuery)
      key |= BITFIELD_BIT(KK_FEAT_MESH_QUERIES);
   return key;
}`)
    .replace('static void\nkk_lower_nir(', '#include "kk_mesh_nir.h"\n\nstatic void\nkk_lower_nir(')
    .replace('   if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {',
      '   kk_lower_task_payload(nir);\n\n   if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {'));
  kk('vulkan/kk_unused_attachments.h', s => s
    .replaceAll('unsigned polygon', 'unsigned kind')
    .replaceAll(/\bpolygon\b/g, 'kind')
    .replace('descriptor[3]', 'descriptor[KK_PIPELINE_KIND_COUNT]')
    .replace('   mtl_render_pipeline_state *original = kind == 2 ? shader->pipeline.gfx.conservative_render\n      : kind ? shader->pipeline.gfx.polygon_render\n                                                : shader->pipeline.gfx.render;',
      `   mtl_render_pipeline_state *original = kind == KK_PIPELINE_MESH_VIRTUAL
      ? shader->mesh_virtual->gfx.render : kind == KK_PIPELINE_MESH_CAPTURE
      ? shader->mesh_retained[0]->gfx.render : kind == KK_PIPELINE_MESH_READER
      ? shader->mesh_retained[1]->gfx.render : kind == KK_PIPELINE_MESH_SLICED
      ? shader->mesh_sliced[0]->gfx.render : kind == KK_PIPELINE_MESH_VIRTUAL_SLICED
      ? shader->mesh_sliced[1]->gfx.render : kind == KK_PIPELINE_CONSERVATIVE
      ? shader->pipeline.gfx.conservative_render : kind == KK_PIPELINE_POLYGON
      ? shader->pipeline.gfx.polygon_render : shader->pipeline.gfx.render;`));
  kk('vulkan/kk_shader.c', s => {
    const start = s.indexOf('   /* If color attachments are reordered');
    const end = s.indexOf('   mtl_render_pipeline_state *render =', start);
    if (start < 0 || end < start) throw Error('shared render descriptor anchors');
    const original = s.slice(start, end);
    const common = original.replace('if (vs->info.vs.view_mask)',
      'if (vs->info.vs.view_mask && vs->info.stage != MESA_SHADER_MESH)');
    s = s.replace(original,
      '   kk_configure_render_pipeline(device, vs, pipe, pipeline_descriptor, polygon);\n\n');
    s = s.replace('static VkResult\nkk_compile_graphics_pipeline_native(',
      `static void
kk_configure_render_pipeline(struct kk_device *device, struct kk_shader *vs,
                             struct kk_pipeline_handles *pipe,
                             mtl_render_pipeline_descriptor *pipeline_descriptor,
                             unsigned polygon)
{
${common}}

static VkResult
kk_record_render_descriptor(struct kk_device *device, struct kk_shader *shader,
                             mtl_render_pipeline_descriptor *descriptor,
                             unsigned kind)
{
   if (!device->vk.enabled_features.dynamicRenderingUnusedAttachments &&
       !device->vk.enabled_features.extendedDynamicState3RasterizationSamples &&
       !device->vk.enabled_features.deviceGeneratedCommands &&
       !device->vk.enabled_features.drawIndirectCount)
      return VK_SUCCESS;
   if (!shader->attachment_pipelines) {
      shader->attachment_pipelines = calloc(1, sizeof(*shader->attachment_pipelines));
      if (!shader->attachment_pipelines)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      simple_mtx_init(&shader->attachment_pipelines->mutex, mtx_plain);
   }
   struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
   simple_mtx_lock(&cache->mutex);
   if (!cache->descriptor[kind])
      cache->descriptor[kind] = mtl_retain(descriptor);
   simple_mtx_unlock(&cache->mutex);
   return VK_SUCCESS;
}

#include "kk_mesh_pipeline.h"

static VkResult
kk_compile_graphics_pipeline_native(`);
    const cache = `   if (render && ((device->vk.enabled_features.dynamicRenderingUnusedAttachments ||
                  device->vk.enabled_features.extendedDynamicState3RasterizationSamples) ||
       device->vk.enabled_features.deviceGeneratedCommands ||
       device->vk.enabled_features.drawIndirectCount)) {
      if (!vs->attachment_pipelines) {
         vs->attachment_pipelines = calloc(1, sizeof(*vs->attachment_pipelines));
         if (!vs->attachment_pipelines)
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
         else
            simple_mtx_init(&vs->attachment_pipelines->mutex, mtx_plain);
      }
      if (vs->attachment_pipelines)
         vs->attachment_pipelines->descriptor[polygon] = mtl_retain(pipeline_descriptor);
   }`;
    return s.replace(cache,
      '   if (render)\n      result = kk_record_render_descriptor(device, vs, pipeline_descriptor, polygon);')
      .replace('kk_compile_graphics_pipeline(struct kk_device *device, struct kk_shader *vs)\n{',
        'kk_compile_graphics_pipeline(struct kk_device *device, struct kk_shader *vs)\n{\n   if (vs->info.stage == MESA_SHADER_MESH)\n      return kk_compile_mesh_pipeline(device, vs, false, &vs->pipeline);');
  });
  kk('vulkan/kk_shader.c', s => s
    .replace('      .per_vertex_map = &per_vertex_map,',
      '      .per_vertex_map = &per_vertex_map,\n      .mesh_queries = features & BITFIELD_BIT(KK_FEAT_MESH_QUERIES),')
    .replace('   gather_shader_info(shader, nir, state);', `   gather_shader_info(shader, nir, state);
   if (stage == MESA_SHADER_TASK || stage == MESA_SHADER_MESH) {
      shader->info.mesh.local_size = (struct mtl_size){
         nir->info.workgroup_size[0], nir->info.workgroup_size[1], nir->info.workgroup_size[2]};
      shader->info.mesh.payload_size = nir->info.task_payload_size;
      shader->info.mesh.queries = opts->mesh_queries;
      if (stage == MESA_SHADER_TASK) {
         shader->info.mesh.task_grid_bound = kk_task_grid_bound(nir);
      } else {
         shader->info.mesh.primitive = nir->info.mesh.primitive_type;
         shader->info.mesh.max_vertices = nir->info.mesh.max_vertices_out;
         shader->info.mesh.view_mask = state && state->mv ? state->mv->view_mask : 0;
         shader->info.uses_per_draw_data |= shader->info.mesh.view_mask != 0;
         shader->info.num_cull_distances = 0;
         if (prev_stage && prev_stage->info.stage == MESA_SHADER_TASK) {
            shader->info.mesh.has_task = true;
            shader->info.mesh.task_local_size = prev_stage->info.mesh.local_size;
            shader->info.mesh.task_payload_size = prev_stage->info.mesh.payload_size;
            shader->info.mesh.task_grid_bound = prev_stage->info.mesh.task_grid_bound;
         }
      }
   }`)
    .replace('   if (stage != MESA_SHADER_GEOMETRY)\n      msl_nir_lower_clip_cull_distance',
      '   if (stage != MESA_SHADER_GEOMETRY && stage != MESA_SHADER_MESH)\n      msl_nir_lower_clip_cull_distance')
    .replace('   struct kk_shader *vs = shaders[0];',
      '   struct kk_shader *vs = shaders[shaders[0]->info.stage == MESA_SHADER_TASK ? 1 : 0];')
    .replace('   if (intr->intrinsic != nir_intrinsic_store_output)\n      return false;\n\n   nir_io_semantics sem =',
      '   if (intr->intrinsic != nir_intrinsic_store_output &&\n       intr->intrinsic != nir_intrinsic_store_per_primitive_output)\n      return false;\n\n   nir_io_semantics sem =')
    .replace('   gs->info.outputs_written |= BITFIELD64_BIT(slot);',
      `   gs->info.outputs_written |= BITFIELD64_BIT(slot);
   if (gs->info.stage == MESA_SHADER_MESH) {
      gs->info.per_primitive_outputs &= ~VARYING_BIT_PRIMITIVE_ID;
      gs->info.per_primitive_outputs |= BITFIELD64_BIT(slot);
      fs->info.per_primitive_inputs |= BITFIELD64_BIT(slot);
   }`)
    .replace('       nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_GEOMETRY) {',
      '       (nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_GEOMETRY ||\n        nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_MESH)) {')
    .replace('   for (uint32_t i = 1; i < shader_count; ++i) {\n      struct kk_shader *s = shaders[i];',
      '   for (uint32_t i = 0; i < shader_count; ++i) {\n      struct kk_shader *s = shaders[i];\n      if (s == vs) continue;')
    .replace('   uint8_t topology = state->ia->primitive_topology;',
      '   uint8_t topology = vs->info.stage == MESA_SHADER_MESH\n      ? (vs->info.mesh.primitive == MESA_PRIM_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST\n         : vs->info.mesh.primitive == MESA_PRIM_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST\n         : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) : state->ia->primitive_topology;')
    .replace('      result = kk_compile_graphics_pipeline(dev, shaders[0]);',
      '      result = kk_compile_graphics_pipeline(dev, shaders[shaders[0]->info.stage == MESA_SHADER_TASK ? 1 : 0]);')
    .replace('   if (stage != MESA_SHADER_VERTEX)\n      return;',
      '   if (stage != MESA_SHADER_VERTEX && stage != MESA_SHADER_MESH)\n      return;\n   if (!shader) return;\n   cmd->state.gfx.shader = shader;')
    .replaceAll('shader->info.stage == MESA_SHADER_VERTEX)',
      '(shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH))')
    .replace('   } else if (info.stage == MESA_SHADER_VERTEX) {',
      '   } else if (info.stage == MESA_SHADER_VERTEX || info.stage == MESA_SHADER_MESH) {')
    .replace('   kk_attachment_pipelines_finish(shader);', `   for (unsigned i = 0; i < 2; ++i)
      if (shader->mesh_retained[i])
         kk_mesh_release_pipeline(dev, shader, shader->mesh_retained[i],
            i ? KK_PIPELINE_MESH_READER : KK_PIPELINE_MESH_CAPTURE);
   if (shader->mesh_virtual)
      kk_mesh_release_pipeline(dev, shader, shader->mesh_virtual, KK_PIPELINE_MESH_VIRTUAL);
   for (unsigned i = 0; i < 2; ++i)
      if (shader->mesh_sliced[i])
         kk_mesh_release_pipeline(dev, shader, shader->mesh_sliced[i],
            i ? KK_PIPELINE_MESH_VIRTUAL_SLICED : KK_PIPELINE_MESH_SLICED);
   free(shader->mesh_fragment);
   kk_attachment_pipelines_finish(shader);`));
  kk('vulkan/kk_cmd_buffer.h', s => s
    .replace('   } metal;',
      '   } metal;\n   struct util_dynarray mesh_command_buffers;\n   struct util_dynarray mesh_allocators;\n   uint32_t mesh_batches_in_part, mesh_draws_in_part;')
    .replace('void cs_end(struct kk_cmd_buffer *cmd);',
      'void cs_end(struct kk_cmd_buffer *cmd);\nbool kk_mesh_split_command_buffer(struct kk_cmd_buffer *cmd);')
    .replace('shaders[MESA_SHADER_STAGES]', 'shaders[MESA_ALL_SHADER_STAGES]')
    .replace('struct kk_graphics_state {',
      'struct kk_graphics_state {\n   struct kk_shader *shader;\n   uint64_t mesh_primitives_query;\n   uint64_t mesh_payloads, mesh_records, mesh_batch, mesh_slices;\n   uint64_t mesh_native_batch, mesh_native_slices;\n   uint64_t mesh_retained_outputs, mesh_retained_output_bytes;\n   uint64_t mesh_per_draw_address;\n   struct kk_mesh_retained_consumer mesh_retained_consumer;')
    .replace('   mtl_argument_table *argument_table;',
      '   mtl_argument_table *argument_table;\n   mtl_argument_table *mesh_table;'));
  kk('vulkan/kk_cmd_buffer.c', s => s
    .replace('   util_dynarray_clear(&cmd->post_render_writes);', `   util_dynarray_foreach(&cmd->mesh_command_buffers, mtl_command_buffer *, part)
      kk_cmd_pool_return_cmd_buf(pool, *part);
   util_dynarray_clear(&cmd->mesh_command_buffers);
   cmd->mesh_batches_in_part = cmd->mesh_draws_in_part = 0;
   util_dynarray_clear(&cmd->post_render_writes);`)
    .replace('   util_dynarray_fini(&cmd->post_render_writes);',
      '   util_dynarray_fini(&cmd->mesh_command_buffers);\n   util_dynarray_fini(&cmd->mesh_allocators);\n   util_dynarray_fini(&cmd->post_render_writes);')
    .replace('   cmd->post_render_writes = UTIL_DYNARRAY_INIT;',
      '   cmd->mesh_command_buffers = UTIL_DYNARRAY_INIT;\n   cmd->mesh_allocators = UTIL_DYNARRAY_INIT;\n   cmd->post_render_writes = UTIL_DYNARRAY_INIT;')
    .replace('   cmd->metal.allocator = NULL;\n}', `   cmd->metal.allocator = NULL;
   util_dynarray_foreach(&cmd->mesh_allocators, mtl_command_allocator *, allocator)
      kk_cmd_pool_return_allocator(kk_cmd_buffer_pool(cmd), *allocator);
   util_dynarray_clear(&cmd->mesh_allocators);
}`)
    .replace('void\nkk_cmd_bind_root_to_argument_table(', `/* Keep long virtual mesh schedules in bounded native command buffers. All
 * parts retain the same Vulkan resources. Separate native allocators bound
 * Metal's command-storage resource index even when most slices execute zero
 * workgroups. Allocators return to the pool only after recording ends;
 * submission order and final query availability are preserved. */
bool
kk_mesh_split_command_buffer(struct kk_cmd_buffer *cmd)
{
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);
   mtl_command_buffer *next = kk_cmd_pool_get_cmd_buf(pool);
   if (!next) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   mtl_command_allocator *allocator = kk_cmd_pool_get_allocator(pool);
   if (!allocator) {
      kk_cmd_pool_return_cmd_buf(pool, next);
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   if (cmd->metal.render)
      kk_apply_attachment_store_ops(cmd, true);
   cs_end(cmd);
   mtl_end_command_buffer(cmd->metal.cmd_buf);
   util_dynarray_append(&cmd->mesh_command_buffers, cmd->metal.cmd_buf);
   util_dynarray_append(&cmd->mesh_allocators, cmd->metal.allocator);
   cmd->metal.cmd_buf = next;
   cmd->metal.allocator = allocator;
   cmd->mesh_batches_in_part = cmd->mesh_draws_in_part = 0;
   mtl_begin_command_buffer(next, allocator);
   if (cmd->vk.base.object_name)
      mtl_command_buffer_set_label(next, cmd->vk.base.object_name);
   cmd->state.gfx.need_to_start_render_pass = true;
   kk_cmd_buffer_dirty_all_gfx(cmd);
   return true;
}

void
kk_cmd_bind_root_to_argument_table(`)
    .replaceAll('stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS',
      'stageFlags & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)')
    .replace('   mtl_release(cmd->argument_table);',
      '   mtl_release(cmd->argument_table);\n   mtl_release(cmd->mesh_table);'));
  kk('vulkan/kk_cmd_buffer.c', s => {
    const wholePassBarrier = `      } else
         if (!kk_whole_pass_attachment_barrier(pDependencyInfo))
            mtl_barrier_after_encoder_stages(cmd->metal.render,
               local ? MTL_STAGE_FRAGMENT : MTL_STAGE_VERTEX, MTL_STAGE_FRAGMENT);`;
    // Preserve the attachment and framebuffer-local decisions of the shared
    // render continuation path. Only nonlocal mesh barriers need more stages.
    if (s.includes(wholePassBarrier))
      return s.replace(wholePassBarrier,
        `      } else if (!kk_whole_pass_attachment_barrier(pDependencyInfo)) {
         bool mesh = cmd->state.gfx.shader &&
            cmd->state.gfx.shader->info.stage == MESA_SHADER_MESH;
         enum mtl_stages before = local ? MTL_STAGE_FRAGMENT : mesh
            ? MTL_STAGE_OBJECTS | MTL_STAGE_MESH | MTL_STAGE_FRAGMENT : MTL_STAGE_VERTEX;
         enum mtl_stages after = mesh && !local ? before : MTL_STAGE_FRAGMENT;
         mtl_barrier_after_encoder_stages(cmd->metal.render, before, after);
      }`);
    return s.replace(`      } else
         mtl_barrier_after_encoder_stages(cmd->metal.render, MTL_STAGE_VERTEX,
                                          MTL_STAGE_FRAGMENT);`,
      `      } else {
         bool mesh = cmd->state.gfx.shader &&
            cmd->state.gfx.shader->info.stage == MESA_SHADER_MESH;
         enum mtl_stages before = mesh
            ? MTL_STAGE_OBJECTS | MTL_STAGE_MESH | MTL_STAGE_FRAGMENT : MTL_STAGE_VERTEX;
         enum mtl_stages after = mesh ? before : MTL_STAGE_FRAGMENT;
         mtl_barrier_after_encoder_stages(cmd->metal.render, before, after);
      }`);
  });
  kk('vulkan/kk_cmd_meta.c', s => s
    .replace('   struct kk_shader *shaders[MESA_SHADER_STAGES];',
      '   struct kk_shader *shaders[MESA_ALL_SHADER_STAGES];\n   struct kk_shader *graphics_shader;\n   uint32_t push_size;')
    .replaceAll('stage < MESA_SHADER_COMPUTE; ++stage) {',
      'stage < MESA_SHADER_MESH_STAGES; ++stage) {\n         if (stage == MESA_SHADER_COMPUTE) continue;')
    .replace('   save->cond_render = cmd->state.cond_render;',
      '   save->graphics_shader = cmd->state.gfx.shader;\n   save->push_size = desc->push_size;\n   save->cond_render = cmd->state.cond_render;')
    .replace('      cmd->state.dirty_shaders |= BITFIELD_MASK(MESA_SHADER_COMPUTE);',
      '      cmd->state.dirty_shaders |= BITFIELD_MASK(MESA_SHADER_MESH_STAGES) &\n         ~BITFIELD_BIT(MESA_SHADER_COMPUTE);\n      cmd->state.gfx.shader = save->graphics_shader;')
    .replace('   cmd->state.cond_render = save->cond_render;',
      '   desc->push_size = save->push_size;\n   cmd->state.cond_render = save->cond_render;'));
  kk('vulkan/kk_cmd_draw.c', s => s
    .replace('kk_rast_prim(struct kk_cmd_buffer *cmd)\n{',
      'kk_rast_prim(struct kk_cmd_buffer *cmd)\n{\n   struct kk_shader *shader = cmd->state.gfx.shader;\n   if (shader && shader->info.stage == MESA_SHADER_MESH)\n      return shader->info.mesh.primitive;')
    .replaceAll('cmd->state.shaders[MESA_SHADER_VERTEX]', 'cmd->state.gfx.shader')
    .replaceAll('IS_SHADER_DIRTY(VERTEX)', '(IS_SHADER_DIRTY(VERTEX) || IS_SHADER_DIRTY(MESH))')
    .replace('#include "kk_statistics_command.h"',
      '#include "kk_statistics_command.h"\n#include "kk_mesh_command.h"'));
  kk('vulkan/kk_vertex_input.h', s => s.replace(
    'cmd->state.shaders[MESA_SHADER_VERTEX]', 'cmd->state.gfx.shader'));
  kk('vulkan/kk_queue.c', s => s
    .replace(`   mtl_command_queue_commit(queue->mtl_handle, &cmd->metal.cmd_buf, 1u,
                            options);`, `   if (util_dynarray_num_elements(&cmd->mesh_command_buffers,
                                  mtl_command_buffer *) > 0) {
      util_dynarray_append(&cmd->mesh_command_buffers, cmd->metal.cmd_buf);
      mtl_command_queue_commit(queue->mtl_handle,
         util_dynarray_begin(&cmd->mesh_command_buffers),
         util_dynarray_num_elements(&cmd->mesh_command_buffers, mtl_command_buffer *),
         options);
      (void)util_dynarray_pop(&cmd->mesh_command_buffers, mtl_command_buffer *);
   } else {
      mtl_command_queue_commit(queue->mtl_handle, &cmd->metal.cmd_buf, 1u,
                               options);
   }`));
  kk('vulkan/kk_query_pool.c', s => s
    .replace('   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT:',
      '   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT:\n   case VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT:')
    .replace('         ~0x7ffu)',
      '         ~(0x7ffu | VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT |\n           VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT))')
    .replace('   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {',
      `   if (pool->vk.query_type == VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT) {
      cmd->state.gfx.mesh_primitives_query = kk_query_report_addr(kk_cmd_buffer_device(cmd), pool, query);
      return;
   }
   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {`)
    .replace('   } else if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {',
      '   } else if (pool->vk.query_type == VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT) {\n      cmd->state.gfx.mesh_primitives_query = 0;\n   } else if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {'));
  kk('vulkan/kk_physical_device.c', s => s
    .replace('      .EXT_multi_draw = true,',
      '      .EXT_multi_draw = true,\n      .EXT_mesh_shader = true,')
    .replace('      .pipelineStatisticsQuery = true,',
      '      .pipelineStatisticsQuery = true,\n      .taskShader = true,\n      .meshShader = true,\n      .meshShaderQueries = true,')
    .replace('      .maxComputeSharedMemorySize =', `      .maxTaskWorkGroupTotalCount = 4194304,
      .maxTaskWorkGroupCount = {65535, 65535, 65535},
      .maxTaskWorkGroupInvocations = 128,
      .maxTaskWorkGroupSize = {128, 128, 128},
      .maxTaskPayloadSize = 16384,
      .maxTaskSharedMemorySize = 32768,
      .maxTaskPayloadAndSharedMemorySize = 32768,
      .maxMeshWorkGroupTotalCount = 4194304,
      .maxMeshWorkGroupCount = {65535, 65535, 65535},
      .maxMeshWorkGroupInvocations = 128,
      .maxMeshWorkGroupSize = {128, 128, 128},
      .maxMeshSharedMemorySize = 28672,
      .maxMeshPayloadAndSharedMemorySize = 28672,
      .maxMeshOutputMemorySize = 32768,
      .maxMeshPayloadAndOutputMemorySize = 48128,
      .maxMeshOutputComponents = 128,
      .maxMeshOutputVertices = 256,
      .maxMeshOutputPrimitives = 512,
      .maxMeshOutputLayers = 8,
      .meshOutputPerVertexGranularity = 1,
      .meshOutputPerPrimitiveGranularity = 1,
      .maxPreferredTaskWorkGroupInvocations = 32,
      .maxPreferredMeshWorkGroupInvocations = 32,
      .maxComputeSharedMemorySize =`));
  kk('bridge/meson.build', s => s.replace("    'mtl_compiler.m',", "    'mtl_compiler.m',\n    'mtl_mesh.m',"));
  kk('bridge/mtl_compiler.h', s => s.replace('/* Compiler */', `mtl_render_pipeline_descriptor *mtl_new_mesh_pipeline_descriptor(
   mtl_function_descriptor *mesh, mtl_function_descriptor *object,
   uint32_t mesh_threads, uint32_t object_threads,
   uint32_t payload_bytes, uint32_t mesh_groups);

/* Compiler */`));
  kk('bridge/mtl_encoder.h', s => s.replace('#endif', `void mtl_draw_mesh_threadgroups(mtl_render_encoder *encoder,
   struct mtl_size groups, struct mtl_size object_size, struct mtl_size mesh_size);
void mtl_draw_mesh_threadgroups_indirect(mtl_render_encoder *encoder,
   uint64_t address, struct mtl_size object_size, struct mtl_size mesh_size);

#endif`));
  kk('bridge/stubs/mtl_compiler.c', s => s + `
mtl_render_pipeline_descriptor *
mtl_new_mesh_pipeline_descriptor(mtl_function_descriptor *mesh,
   mtl_function_descriptor *object, uint32_t mesh_threads,
   uint32_t object_threads, uint32_t payload_bytes, uint32_t mesh_groups)
{
   return NULL;
}
`);
  kk('bridge/stubs/mtl_encoder.c', s => s + `
void mtl_draw_mesh_threadgroups(mtl_render_encoder *encoder,
   struct mtl_size groups, struct mtl_size object_size, struct mtl_size mesh_size) {}
void mtl_draw_mesh_threadgroups_indirect(mtl_render_encoder *encoder,
   uint64_t address, struct mtl_size object_size, struct mtl_size mesh_size) {}
`);
  kk('libkk/meson.build', s => s.replace("  'kk_draws.cl',", "  'kk_draws.cl',\n  'kk_mesh.cl',"));
  kk('compiler/msl_private.h', s => s.replace('   bool uses_per_draw_data;',
    '   bool uses_per_draw_data;\n   bool mesh_queries;'));
  kk('compiler/msl_iomap.c', s => s
    .replace('   case nir_intrinsic_load_input: {',
      '   case nir_intrinsic_load_per_primitive_input:\n   case nir_intrinsic_load_input: {')
    .replace('static bool\nmsl_nir_gather_io_info(', '#include "kk_mesh_iomap.h"\n\nstatic bool\nmsl_nir_gather_io_info(')
    .replace('   struct gather_ctx *ctx = (struct gather_ctx *)data;', `   struct gather_ctx *ctx = (struct gather_ctx *)data;
   if (b->shader->info.stage == MESA_SHADER_MESH &&
       kk_mesh_gather_io(intrin, ctx->output))
      return false;`)
    .replace(`   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TESS_CTRL:
      break;`, `   case MESA_SHADER_MESH:
      kk_mesh_io_blocks(ctx);
      break;
   case MESA_SHADER_TASK:
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TESS_CTRL:
      break;`)
    .replace(`   if (load_from_input) {
      P(ctx, "in.");
   } else {
      P(ctx, "out.");
   }`, `   if (ctx->shader->info.stage != MESA_SHADER_MESH) {
      if (load_from_input) {
         P(ctx, "in.");
      } else {
         P(ctx, "out.");
      }
   }`)
    .replace('   case MESA_SHADER_VERTEX:\n      varying_slot_name(ctx, location, component);',
      '   case MESA_SHADER_VERTEX:\n   case MESA_SHADER_MESH:\n      varying_slot_name(ctx, location, component);')
    .replace('   if (ctx->shader->info.stage == MESA_SHADER_VERTEX &&\n       VARYING_SLOT_INFO[location].scalarized)',
      '   if ((ctx->shader->info.stage == MESA_SHADER_VERTEX ||\n        ctx->shader->info.stage == MESA_SHADER_MESH) &&\n       VARYING_SLOT_INFO[location].scalarized)'));
  kk('compiler/msl_type_inference.c', s => s
    .replace('   case nir_intrinsic_load_input:',
      '   case nir_intrinsic_load_per_primitive_input:\n   case nir_intrinsic_load_input:')
    .replace('   case nir_intrinsic_store_output: {', `   case nir_intrinsic_set_vertex_and_primitive_count:
      set_type(types, &instr->src[0], TYPE_UINT);
      set_type(types, &instr->src[1], TYPE_UINT);
      break;
   case nir_intrinsic_launch_mesh_workgroups:
      set_type(types, &instr->src[0], TYPE_UINT);
      break;
   case nir_intrinsic_store_output: {`)
    .replace('   case nir_intrinsic_store_output: {', `   case nir_intrinsic_load_per_vertex_output:
   case nir_intrinsic_load_per_primitive_output:
      set_type(types, &instr->def, ti_type_from_nir(nir_intrinsic_dest_type(instr)));
      set_type(types, &instr->src[0], TYPE_UINT);
      set_type(types, &instr->src[1], TYPE_UINT);
      break;
   case nir_intrinsic_store_per_vertex_output:
   case nir_intrinsic_store_per_primitive_output:
      set_type(types, &instr->src[0], ti_type_from_nir(nir_intrinsic_src_type(instr)));
      set_type(types, &instr->src[1], TYPE_UINT);
      set_type(types, &instr->src[2], TYPE_UINT);
      break;
   case nir_intrinsic_store_output: {`)
    .replaceAll('case nir_intrinsic_load_shared:', 'case nir_intrinsic_load_shared:\n   case nir_intrinsic_load_task_payload:')
    .replaceAll('case nir_intrinsic_store_shared:', 'case nir_intrinsic_store_shared:\n   case nir_intrinsic_store_task_payload:'));
  kk('compiler/nir_to_msl.c', s => s
    .replace('   case nir_intrinsic_load_input: {',
      '   case nir_intrinsic_load_per_primitive_input:\n   case nir_intrinsic_load_input: {')
    .replace('   case nir_jump_halt:\n   case nir_jump_abort:',
      `   case nir_jump_halt:
      if (ctx->shader->info.stage == MESA_SHADER_TASK) {
         P_IND(ctx, "return;\\n");
         break;
      }
      FALLTHROUGH;
   case nir_jump_abort:`)
    .replace('static void\nintrinsic_to_msl(', '#include "kk_mesh_runtime.h"\n#include "kk_mesh_intrinsics.h"\n\nstatic void\nintrinsic_to_msl(')
    .replace('   if (kk_emit_atomic64(ctx, instr)) return;',
      '   if (kk_emit_mesh_intrinsic(ctx, instr)) return;\n   if (kk_emit_atomic64(ctx, instr)) return;')
    .replace('      case nir_var_mem_shared:\n         P(ctx, "mem_flags::mem_threadgroup");',
      '      case nir_var_mem_task_payload:\n         P(ctx, "kk_payload_flags(payload)");\n         break;\n      case nir_var_shader_out:\n         P(ctx, "mem_flags::mem_threadgroup");\n         break;\n      case nir_var_mem_shared:\n         P(ctx, "mem_flags::mem_threadgroup");')
    .replace('       ctx->shader->info.stage == MESA_SHADER_FRAGMENT) {\n      msl_gather_io_info',
      '       ctx->shader->info.stage == MESA_SHADER_FRAGMENT ||\n       ctx->shader->info.stage == MESA_SHADER_MESH) {\n      msl_gather_io_info')
    .replace('char *\nnir_to_msl(', '#include "kk_mesh_msl.h"\n\nchar *\nnir_to_msl(')
    .replace('      .disabled_workarounds = options->disabled_workarounds,',
      '      .disabled_workarounds = options->disabled_workarounds,\n      .mesh_queries = options->mesh_queries,')
    .replace('   msl_emit_constant_data(&ctx, shader);',
      '   if (shader->info.stage == MESA_SHADER_TASK || shader->info.stage == MESA_SHADER_MESH)\n      P(&ctx, "%s", kk_mesh_runtime_msl);\n\n   msl_emit_constant_data(&ctx, shader);')
    .replace('   P(&ctx, "%s %s %s(\\n", get_stage_string(shader->info.stage),',
      '   if (shader->info.stage == MESA_SHADER_TASK || shader->info.stage == MESA_SHADER_MESH) {\n      kk_mesh_emit_functions(&ctx, impl);\n   } else {\n   P(&ctx, "%s %s %s(\\n", get_stage_string(shader->info.stage),')
    .replace('   char *ret = ctx.text->buf;', '   }\n   if (shader->info.stage == MESA_SHADER_MESH && options->mesh_retained_strides) {\n      options->mesh_retained_strides[0] = ctx.mesh_retained_vertex_stride;\n      options->mesh_retained_strides[1] = ctx.mesh_retained_primitive_stride;\n   }\n   if (shader->info.stage == MESA_SHADER_MESH && options->mesh_retained_fields)\n      memcpy(options->mesh_retained_fields, ctx.mesh_retained_fields, sizeof(ctx.mesh_retained_fields));\n   char *ret = ctx.text->buf;'));
  kk('vulkan/kk_shader.h', s => s.replace('   bool has_task;',
    '   bool has_task;\n   bool flat_heap;\n   bool projected_basis;\n   uint32_t max_primitives;'));
  kk('compiler/nir_to_msl.h', s => s.replace('   bool mesh_queries;',
    '   bool mesh_queries;\n   uint32_t *mesh_retained_strides;\n   uint32_t *mesh_retained_fields;\n   uint32_t mesh_view_mask, mesh_view_index_offset;\n   bool mesh_flat_heap;\n   bool mesh_projected_basis;\n   unsigned mesh_flat_heap_offset;'));
  kk('compiler/msl_private.h', s => s.replace('   bool mesh_queries;',
    '   bool mesh_queries;\n   uint32_t mesh_retained_vertex_stride, mesh_retained_primitive_stride;\n   uint32_t mesh_retained_fields[4][4];\n   uint32_t mesh_view_mask, mesh_view_index_offset;\n   bool mesh_flat_heap;\n   bool mesh_projected_basis;\n   unsigned mesh_flat_heap_offset;'));
  kk('compiler/nir_to_msl.c', s => s
    .replace('      .mesh_queries = options->mesh_queries,',
      '      .mesh_queries = options->mesh_queries,\n      .mesh_view_mask = options->mesh_view_mask,\n      .mesh_view_index_offset = options->mesh_view_index_offset,\n      .mesh_flat_heap = options->mesh_flat_heap,\n      .mesh_projected_basis = options->mesh_projected_basis,\n      .mesh_flat_heap_offset = options->mesh_flat_heap_offset,')
    .replace('   ctx->uses_per_draw_data = msl_gather_uses_per_draw_data(ctx->shader);',
      '   ctx->uses_per_draw_data = ctx->mesh_flat_heap || ctx->mesh_view_mask ||\n      msl_gather_uses_per_draw_data(ctx->shader);'));
  kk('vulkan/kk_shader.c', s => s
    .replace('   translate_options.mesh_queries = opts && opts->mesh_queries;',
      `   translate_options.mesh_queries = opts && opts->mesh_queries;
   translate_options.mesh_retained_strides = opts ? opts->mesh_retained_strides : NULL;
   translate_options.mesh_retained_fields = opts ? opts->mesh_retained_fields : NULL;
   translate_options.mesh_view_mask = nir->info.stage == MESA_SHADER_MESH && state && state->mv ? state->mv->view_mask : 0;
   translate_options.mesh_view_index_offset = offsetof(struct kk_per_draw_data, multiview_index);
   translate_options.mesh_flat_heap = nir->info.stage == MESA_SHADER_MESH &&
      opts && opts->per_vertex_map && opts->per_vertex_map->active &&
      opts->per_vertex_map->canonical_heap;
   translate_options.mesh_projected_basis = translate_options.mesh_flat_heap &&\n      opts->per_vertex_map->projected_basis;
   translate_options.mesh_flat_heap_offset = translate_options.mesh_flat_heap
      ? offsetof(struct kk_per_draw_data, per_vertex_heap) : 0;`)
    .replace('         shader->info.mesh.primitive = nir->info.mesh.primitive_type;',
      `         shader->info.mesh.primitive = nir->info.mesh.primitive_type;
         shader->info.mesh.flat_heap = per_vertex_map && per_vertex_map->active &&
            per_vertex_map->canonical_heap;
         shader->info.mesh.projected_basis = shader->info.mesh.flat_heap &&
            per_vertex_map->projected_basis;
         shader->info.mesh.max_primitives = nir->info.mesh.max_primitives_out;
         shader->info.uses_per_draw_data |= shader->info.mesh.flat_heap;`)
    .replace('   struct kk_per_vertex_map per_vertex_map = {0};',
      `   struct kk_per_vertex_map per_vertex_map = {0};
   if (total_shaders >= 2u &&
       nir_shaders[total_shaders - 1u]->info.stage == MESA_SHADER_FRAGMENT &&
       nir_shaders[total_shaders - 2u]->info.stage == MESA_SHADER_MESH)
      NIR_PASS(_, nir_shaders[total_shaders - 1u], kk_nir_lower_mesh_flat_inputs,
               nir_shaders[total_shaders - 2u], &per_vertex_map);`));
  kk('vulkan/kk_canonical_per_vertex.h', s => {
    const count = `#define KK_CANONICAL_VARYING_COUNT \\
   (VARYING_SLOT_VAR31 - VARYING_SLOT_VAR0 + 1)`;
    return s.replace(count, '#include "kosmickrisp/libkk/kk_canonical_layout.h"');
  });
  kk('vulkan/kk_nir_lower_per_vertex.h', s => s
    .replace('   bool canonical_heap;', '   bool canonical_heap;\n   bool projected_basis;')
    .replace(
    'bool kk_nir_lower_vs_per_vertex_outputs(', `
bool kk_nir_lower_mesh_flat_inputs(nir_shader *fs, nir_shader *mesh,
                                   struct kk_per_vertex_map *map);

bool kk_nir_lower_vs_per_vertex_outputs(`));
  kk('vulkan/kk_nir_lower_per_vertex.c', s => s
    .replace('bool flat = intr->intrinsic == nir_intrinsic_load_input;',
      'bool flat = intr->intrinsic == nir_intrinsic_load_input ||\n                     intr->intrinsic == nir_intrinsic_load_per_primitive_input;')
    .replaceAll('intr->intrinsic != nir_intrinsic_load_input)',
      'intr->intrinsic != nir_intrinsic_load_input &&\n             intr->intrinsic != nir_intrinsic_load_per_primitive_input)')
    .replace('         } else if ((intr->intrinsic == nir_intrinsic_load_input ||',
      '         } else if ((intr->intrinsic == nir_intrinsic_load_input ||\n                     intr->intrinsic == nir_intrinsic_load_per_primitive_input ||')
    .replace('intr->intrinsic == nir_intrinsic_load_input ? 0 : 1',
      'intr->intrinsic == nir_intrinsic_load_interpolated_input ? 1 : 0')
    .replace('static nir_def *\nload_per_vertex_value(',
      'static nir_def *load_per_draw(nir_builder *, unsigned, unsigned);\n#include "kk_canonical_projected.h"\n\nstatic nir_def *\nload_per_vertex_value(')
    .replace('   enum glsl_interp_mode mode = nir_intrinsic_interp_mode(intr);',
      '   if (map->projected_basis)\n      return kk_canonical_projected_weights(b, intr);\n   enum glsl_interp_mode mode = nir_intrinsic_interp_mode(intr);')
    .replace('   nir_def *weights = nir_load_interpolated_input(',
      '   nir_def *weights = map->projected_basis ?\n      kk_canonical_projected_weights(b, original) : nir_load_interpolated_input(')
    .replace('   memset(map, 0, sizeof(*map));\n   bool uses_barycentric[2]',
      '   bool projected = map->projected_basis;\n   memset(map, 0, sizeof(*map));\n   bool uses_barycentric[2]')
    .replace('   nir_builder b = nir_builder_create(impl);',
      '   if (projected) {\n      map->projected_basis = true;\n      map->barycentric_slot[0] = map->barycentric_slot[1] = KK_CANONICAL_PROJECTED_POINT;\n   }\n   nir_builder b = nir_builder_create(impl);')
    .replace('      kk_canonical_map_init(&canonical);',
      '      kk_canonical_map_init(&canonical);\n      canonical.projected_basis = map->projected_basis;\n      if (canonical.projected_basis)\n         canonical.barycentric_slot[0] = canonical.barycentric_slot[1] = KK_CANONICAL_PROJECTED_POINT;')
    .replace('            if ((map->locations & BITFIELD64_BIT(location)) && map->buffer) {',
      '            if (map->projected_basis && location == VARYING_SLOT_POS) {\n               nir_builder b = nir_builder_at(nir_before_instr(instr));\n               kk_canonical_store_corner(&b, intr, corner, load_param(nir, instr, 2));\n               nir_instr_remove(instr);\n            } else if ((map->locations & BITFIELD64_BIT(location)) && map->buffer) {')
    .replace('         } else if (intr->intrinsic == nir_intrinsic_store_output &&\n                    ((map->locations',
      '         } else if (map->projected_basis && intr->intrinsic == nir_intrinsic_store_output &&\n                    io_location(intr, 1) == VARYING_SLOT_POS) {\n            nir_builder carrier = nir_builder_at(nir_before_instr(instr));\n            kk_canonical_store_position(&carrier, intr, KK_CANONICAL_PROJECTED_POINT);\n         } else if (intr->intrinsic == nir_intrinsic_store_output &&\n                    ((map->locations')
    .replace('      unsigned size = 3u * count * 16u;', '      unsigned size = map->projected_basis ? kk_canonical_projected_buffer_size() : 3u * count * 16u;')
    .replace('      if (!map->barycentric_slot[i])\n         continue;',
      '      if (!map->barycentric_slot[i] || map->projected_basis)\n         continue;')
    .replace('   unsigned components = sample_id + sample_mask + front_face +',
      `   /* Native mesh position contributes three additional packed interface
    * components on the measured Apple mesh path (124 generic reports127).
    * This is transport pressure, not a reduction of Vulkan output limits. */
   unsigned mesh_position = prev && prev->info.stage == MESA_SHADER_MESH ? 3u : 0u;
   unsigned components = mesh_position + sample_id + sample_mask + front_face +`)
    .replace(`   unsigned count = map->canonical_heap ? KK_CANONICAL_VARYING_COUNT
                                        : util_bitcount64(map->locations);
   unsigned index = map->canonical_heap ? location - VARYING_SLOT_VAR0
      : util_bitcount64(map->locations & BITFIELD64_MASK(location));`,
      `   if (map->canonical_heap)
      return kk_canonical_buffer_offset(location, corner);
   unsigned count = util_bitcount64(map->locations);
   unsigned index = util_bitcount64(map->locations & BITFIELD64_MASK(location));`)
    + `
/* Mesh outputs are already staged after its collective body. Flat interfaces
 * need the shared address packet, without the VS producer's corner replay. */
bool
kk_nir_lower_mesh_flat_inputs(nir_shader *fs, nir_shader *mesh,
                              struct kk_per_vertex_map *map)
{
   assert(mesh->info.stage == MESA_SHADER_MESH);
   if (!kk_nir_needs_interface_packing(fs, mesh))
      return false;
   nir_foreach_block(block, nir_shader_get_entrypoint(fs)) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (is_barycentric_coord(intr) || is_per_vertex_input(intr) ||
             (intr->intrinsic == nir_intrinsic_load_interpolated_input &&
              (kk_canonical_generic_mask() & BITFIELD64_BIT(io_location(intr, 1)))))
            map->projected_basis = true;
      }
   }
   return kk_nir_lower_fs_per_vertex_inputs(fs, NULL, true, map);
}
`);
  kk('vulkan/kk_canonical_per_vertex.h', s => s.replace(
    'bool flat = intr->intrinsic == nir_intrinsic_load_input;',
    'bool flat = intr->intrinsic == nir_intrinsic_load_input ||\n                     intr->intrinsic == nir_intrinsic_load_per_primitive_input;'));
  for (const [source, target] of [
    ['kosmickrisp-canonical-projected.h', 'vulkan/kk_canonical_projected.h'],
    ['kosmickrisp-mesh-types.h', 'libkk/kk_mesh_types.h'],
    ['kosmickrisp-mesh-dispatch.h', 'libkk/kk_mesh_dispatch.h'],
    ['kosmickrisp-mesh-sliced.h', 'vulkan/kk_mesh_sliced.h'],
    ['kosmickrisp-mesh-api.h', 'vulkan/kk_mesh_pipeline_api.h'],
    ['kosmickrisp-mesh-nir.h', 'vulkan/kk_mesh_nir.h'],
    ['kosmickrisp-mesh.cl', 'libkk/kk_mesh.cl'],
    ['kosmickrisp-mesh-kernels.h', 'libkk/kk_mesh_kernels.h'],
    ['kosmickrisp-mesh-iomap.h', 'compiler/kk_mesh_iomap.h'],
    ['kosmickrisp-mesh-intrinsics.h', 'compiler/kk_mesh_intrinsics.h'],
    ['kosmickrisp-mesh-msl.h', 'compiler/kk_mesh_msl.h'],
    ['kosmickrisp-mesh-retained-msl.h', 'compiler/kk_mesh_retained_msl.h'],
    ['kosmickrisp-mesh-bridge.m', 'bridge/mtl_mesh.m'],
    ['kosmickrisp-mesh-pipeline.h', 'vulkan/kk_mesh_pipeline.h'],
    ['kosmickrisp-mesh-command.h', 'vulkan/kk_mesh_command.h'],
    ['kosmickrisp-canonical-layout.h', 'libkk/kk_canonical_layout.h'],
    ['kosmickrisp-mesh-canonical.h', 'compiler/kk_mesh_canonical.h'],
  ]) edits.set('src/kosmickrisp/' + target,
    fs.readFileSync(new URL('./' + source, import.meta.url), 'utf8'));
  const runtime = fs.readFileSync(new URL('./kosmickrisp-mesh-types.h', import.meta.url), 'utf8') + '\n' +
    fs.readFileSync(new URL('./kosmickrisp-mesh.metal', import.meta.url), 'utf8');
  edits.set('src/kosmickrisp/compiler/kk_mesh_runtime.h',
    '/* SPDX-License-Identifier: MIT */\nstatic const char kk_mesh_runtime_msl[] = ' + JSON.stringify(runtime) + ';\n');
  // Reuse the named nullable Poly selector after input attachments have
  // introduced any Layer loads, before descriptor lowering.
  kk('vulkan/kk_shader.c', s => s
    .replace('#include "kk_mesh_nir.h"', '#include "kk_mesh_nir.h"\nstatic void kk_poly_layer_lower_fs(nir_shader *nir);')
    .replace('   /* Common msl texture lowering needs to happen after ycbcr lowering and',
      '   if (nir->info.stage == MESA_SHADER_FRAGMENT &&\n       (state->shader_stages & VK_SHADER_STAGE_MESH_BIT_EXT))\n      kk_poly_layer_lower_fs(nir);\n\n   /* Common msl texture lowering needs to happen after ycbcr lowering and'));
  kk('vulkan/kk_compile_options.h', s => s.replace('   bool mesh_queries;',
    '   bool mesh_queries;\n   uint32_t *mesh_retained_strides;\n   uint32_t *mesh_retained_fields;'));
  kk('vulkan/kk_shader.c', s => s.replace(
    '   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts,',
    '   struct kk_compile_options mesh_options = *opts;\n   if (stage == MESA_SHADER_MESH) {\n      mesh_options.mesh_retained_strides = shader->info.mesh.retained_strides;\n      mesh_options.mesh_retained_fields = (uint32_t *)shader->info.mesh.retained_fields;\n      opts = &mesh_options;\n   }\n   kk_compile_msl_options(pdev, nir, state, &shader->msl_data[stage], opts,'));

  kk('vulkan/kk_shader.c', s => s.replace(
    '   /* Massage IO related variables to please Metal */',
    '   if (nir->info.stage == MESA_SHADER_TASK || nir->info.stage == MESA_SHADER_MESH)\n      NIR_PASS(_, nir, kk_nir_lower_poly_multiview, state && state->mv && state->mv->view_mask, false);\n\n   /* Massage IO related variables to please Metal */'));

}
