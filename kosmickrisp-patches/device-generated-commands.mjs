// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'device-generated-commands';
export const sourceFiles = ['kk_dgc.h','kk_dgc.m','kk_dgc.metal','kk_cmd_dgc.c','kk_dgc_indirect.h','kk_dgc_poly.h','kk_dgc_pipeline.h','kk_dgc.cl'].map(f=>'kosmickrisp-patches/'+f);
export function apply({edit,edits}) {
  const changeFunction = (s, name, from, to) => {
    const start = s.indexOf(name + '(');
    if (start < 0) throw Error('Missing DGC command anchor: ' + name);
    let end = s.indexOf('\nVKAPI_ATTR', start);
    if (end < 0) end = s.length;
    const body = s.slice(start, end);
    if (!body.includes(from)) throw Error('Missing DGC body anchor in ' + name);
    return s.replace(body, body.replace(from, to));
  };
  for(const file of ['kk_dgc.h','kk_dgc.m','kk_cmd_dgc.c','kk_dgc_indirect.h','kk_dgc_poly.h','kk_dgc_pipeline.h'])
    edits.set('src/kosmickrisp/vulkan/'+file,fs.readFileSync(new URL('./'+file,import.meta.url),'utf8'));
  edits.set('src/kosmickrisp/libkk/kk_dgc.cl',fs.readFileSync(new URL('./kk_dgc.cl',import.meta.url),'utf8'));
  edit('src/kosmickrisp/libkk/meson.build',s=>s.replace("  'kk_draws.cl',","  'kk_draws.cl',\n  'kk_dgc.cl',"));
  const shader=fs.readFileSync(new URL('./kk_dgc.metal',import.meta.url),'utf8');
  edits.set('src/kosmickrisp/vulkan/kk_dgc_shader.h','/* SPDX-License-Identifier: MIT */\nstatic const char kk_dgc_shader[] = '+JSON.stringify(shader)+';\n');
  edit('src/kosmickrisp/vulkan/meson.build',s=>s.replace("  'kk_cmd_dispatch.c',","  'kk_cmd_dispatch.c',\n  'kk_cmd_dgc.c',\n  'kk_dgc.m',"));
  edit('src/kosmickrisp/vulkan/kk_device.h',s=>s.replace('   mtl_device *mtl_handle;','   struct kk_dgc_metal *dgc;\n   mtl_device *mtl_handle;'));
  edit('src/kosmickrisp/vulkan/kk_device.c',s=>s.replace('#include "kk_device.h"','#include "kk_device.h"\n#include "kk_dgc.h"')
    .replace('   *pDevice = kk_device_to_handle(dev);',`   if (dev->vk.enabled_features.deviceGeneratedCommands && !kk_dgc_init(dev)) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      kk_device_finish_lib(dev);
      goto fail_sampler_heap;
   }
   *pDevice = kk_device_to_handle(dev);`)
    .replaceAll('   kk_device_finish_lib(dev);\n','   kk_dgc_finish(dev);\n   kk_device_finish_lib(dev);\n'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h',s=>s.replace('   struct kk_per_draw_data per_draw_data;','   struct kk_per_draw_data per_draw_data;\n   uint32_t indirect_draw_id;').replace('   uint32_t indirect_draw_id;', '   uint32_t indirect_draw_id;\n   uint64_t generated_root;\n   uint64_t generated_heap_floor;').replace('   struct util_dynarray large_bos;','   struct util_dynarray large_bos;\n   struct util_dynarray dgc_commands;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c',s=>s.replace('#include "kk_cmd_buffer.h"','#include "kk_cmd_buffer.h"\n#include "kk_dgc.h"')
    .replace('   kk_cmd_release_dynamic_ds_state(cmd);','   kk_dgc_release_commands(cmd);\n   kk_cmd_release_dynamic_ds_state(cmd);')
    .replace('   cmd->large_bos = UTIL_DYNARRAY_INIT;','   cmd->large_bos = UTIL_DYNARRAY_INIT;\n   cmd->dgc_commands = UTIL_DYNARRAY_INIT;')
    .replace('   util_dynarray_fini(&cmd->large_bos);','   util_dynarray_fini(&cmd->large_bos);\n   util_dynarray_fini(&cmd->dgc_commands);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c',s=>s.replace('#include \"kk_cmd_buffer.h\"','#include \"kk_cmd_buffer.h\"\n#include \"kk_dgc.h\"').replace('VKAPI_ATTR void VKAPI_CALL\nkk_CmdDrawIndirectCount2KHR',`/* DGC uses the same pipeline, attachment and dynamic state flush as draws. */
void kk_dgc_flush_graphics(struct kk_cmd_buffer *cmd) { kk_flush_gfx_state(cmd); }
uint32_t kk_dgc_primitive(struct kk_cmd_buffer *cmd) { return mesa_prim_to_mtl_primitive_type(vk_topology_to_mesa(cmd->vk.dynamic_graphics_state.ia.primitive_topology)); }

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndirectCount2KHR`));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c',s=>{
    for(const [name,indexed] of [['kk_CmdDrawIndirectCount2KHR',false],['kk_CmdDrawIndexedIndirectCount2KHR',true]]) {
      s=changeFunction(s,name,'   const struct vk_dynamic_graphics_state *dyn =',        '   if (pInfo->maxDrawCount >= 1024 &&\n' +
        '       !cmd->state.shaders[MESA_SHADER_TESS_EVAL] && !kk_geometry_shader(cmd) &&\n' +
        '       cmd->vk.dynamic_graphics_state.rs.polygon_mode == VK_POLYGON_MODE_FILL &&\n' +
        '       !cmd->state.gfx.xfb_enabled && !cmd->state.pipeline_statistics &&\n' +
        '       !cmd->state.gfx.primitives_query[0] &&\n' +
        '       (!'+indexed+' || (cmd->state.gfx.index.bytes_per_index >= 2 &&\n' +
        '        !cmd->vk.dynamic_graphics_state.ia.primitive_restart_enable)) &&\n' +
        '       !mesa_prim_has_adjacency(vk_topology_to_mesa(cmd->vk.dynamic_graphics_state.ia.primitive_topology)) &&\n' +
        '       cmd->vk.dynamic_graphics_state.ia.primitive_topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {\n' +
        '      kk_dgc_draw_indirect_count(cmd,pInfo,'+indexed+');\n      return;\n   }\n\n' +
        '   const struct vk_dynamic_graphics_state *dyn =');
    }
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c',s=>{
    s=s.replace('data->upload_mask, i, &draw_data,','data->upload_mask, i + gfx->indirect_draw_id, &draw_data,');
    s=s.replace('data->indirect_command.addr + draw_id * data->indirect_command.stride','data->indirect_command.addr + (uint64_t)draw_id * data->indirect_command.stride');
    s=s.replace('VKAPI_ATTR void VKAPI_CALL\nkk_CmdDrawIndirect2KHR','#include "kk_dgc_indirect.h"\n\nVKAPI_ATTR void VKAPI_CALL\nkk_CmdDrawIndirect2KHR');
    for(const name of ['kk_CmdDrawIndirect2KHR','kk_CmdDrawIndexedIndirect2KHR','kk_CmdDrawIndirectCount2KHR','kk_CmdDrawIndexedIndirectCount2KHR']) {
      s=changeFunction(s,name,'   kk_draw(cmd, &data);','   if (data.draw_count > 4096)\n      kk_draw_large_indirect(cmd, &data);\n   else\n      kk_draw(cmd, &data);');
    }
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s.replace(
`uint64_t
kk_upload_descriptor_root(struct kk_cmd_buffer *cmd,
                          VkPipelineBindPoint bind_point)
{
   struct kk_descriptor_state *desc = kk_get_descriptors_state(cmd, bind_point);
   struct kk_root_descriptor_table *root = &desc->root;
   uint32_t size = offsetof(struct kk_root_descriptor_table, push) + desc->push_size;
   struct kk_ptr root_ptr = kk_pool_alloc(cmd, size, 8u);
   if (unlikely(!root_ptr.gpu))
      return 0u;

   root->addr = root_ptr.gpu;

   memcpy(root_ptr.cpu, root, size);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if (dev->atomic64_locks)
      ((struct kk_root_descriptor_table *)root_ptr.cpu)->addr = dev->atomic64_locks->gpu;
   desc->root_dirty = false;

   return root_ptr.gpu;
}`,
`uint64_t
kk_upload_descriptor_root_data(struct kk_cmd_buffer *cmd,
   const struct kk_root_descriptor_table *root, uint32_t size)
{
   struct kk_ptr ptr = kk_pool_upload(cmd, root, size, 8u);
   if (unlikely(!ptr.gpu)) return 0u;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   ((struct kk_root_descriptor_table *)ptr.cpu)->addr =
      dev->atomic64_locks ? dev->atomic64_locks->gpu : ptr.gpu;
   return ptr.gpu;
}

uint64_t
kk_upload_descriptor_root(struct kk_cmd_buffer *cmd,
                          VkPipelineBindPoint bind_point)
{
   struct kk_descriptor_state *desc = kk_get_descriptors_state(cmd, bind_point);
   struct kk_root_descriptor_table *root = &desc->root;
   uint32_t size = offsetof(struct kk_root_descriptor_table, push) + desc->push_size;
   uint64_t address = kk_upload_descriptor_root_data(cmd, root, size);
   if (!address) return 0u;
   root->addr = address;
   desc->root_dirty = false;
   return address;
}`));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    'uint64_t kk_upload_descriptor_root(struct kk_cmd_buffer *cmd,',
    'uint64_t kk_upload_descriptor_root_data(struct kk_cmd_buffer *cmd,\n   const struct kk_root_descriptor_table *root, uint32_t size);\n\nuint64_t kk_upload_descriptor_root(struct kk_cmd_buffer *cmd,'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c',s=>s.replace(
    '   struct kk_root_descriptor_table *root = &desc->root;',
    '   struct kk_root_descriptor_table *root = &desc->root;\n   if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS && cmd->state.gfx.generated_root) {\n      root->addr = cmd->state.gfx.generated_root;\n      desc->root_dirty = false;\n      return root->addr;\n   }'));
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c',s=>s.replace(
    '      kk_cmd_write(cmd, (struct libkk_imm_write){\n                           addr + offsetof(struct poly_heap, bottom), 0});',
    '      if (cmd->state.gfx.generated_heap_floor) {\n         libkk_dgc_reserve_heap(cmd, kk_grid_1d(1), true, addr, cmd->state.gfx.generated_heap_floor);\n      } else {\n         kk_cmd_write(cmd, (struct libkk_imm_write){addr + offsetof(struct poly_heap, bottom), 0});\n      }')
    .replace('#include "kk_dgc_indirect.h"','#include "kk_dgc_poly.h"\n#include "kk_dgc_indirect.h"'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c',s=>s
    .replace('   .EXT_extended_dynamic_state = true,','   .EXT_device_generated_commands = true,\n   .EXT_extended_dynamic_state = true,')
    .replace('      .dynamicRendering = true,','      .dynamicRendering = true,\n      .deviceGeneratedCommands = true,\n      .dynamicGeneratedPipelineLayout = true,')
    .replace('      .maxDrawIndirectCount = UINT16_MAX,','      .maxDrawIndirectCount = 1u << 30,')
    .replace('      .maxPushConstantsSize = KK_MAX_PUSH_SIZE,',`      .maxPushConstantsSize = KK_MAX_PUSH_SIZE,
      .maxIndirectPipelineCount = 0,
      .maxIndirectShaderObjectCount = 0,
      .maxIndirectSequenceCount = 1u << 20,
      .maxIndirectCommandsTokenCount = 128,
      .maxIndirectCommandsTokenOffset = 4095,
      .maxIndirectCommandsIndirectStride = 4096,
      .supportedIndirectCommandsInputModes = VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT | VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT,
      .supportedIndirectCommandsShaderStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
      .supportedIndirectCommandsShaderStagesPipelineBinding = 0,
      .supportedIndirectCommandsShaderStagesShaderBinding = 0,
      .deviceGeneratedCommandsTransformFeedback = false,
      .deviceGeneratedCommandsMultiDrawIndirectCount = false,`));
  applyPipelines({edit,edits});
}

function applyPipelines({edit}) {
  const checked = (s, from, to) => {
    if (!s.includes(from)) throw Error('Missing DGC pipeline anchor: ' + from.slice(0, 100));
    return s.replace(from, to);
  };
  edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
    .replace('   uint32_t push_size;', '   uint32_t push_size;\n   bool dgc_replay;\n   bool indirect_bindable;')
    .replace('   struct kk_pipeline_handles pipeline;', '   mtl_compute_pipeline_state *dgc_compute;\n   struct kk_pipeline_handles pipeline;'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s.replace(
    '   uint64_t generated_root;', '   bool dgc_icb;\n   uint64_t generated_root;'));
  const linking = `void\nmtl_render_pipeline_descriptor_copy_linking(\n   mtl_render_pipeline_descriptor *copy, mtl_render_pipeline_descriptor *source)\n{\n   /* Copies pipeline-linking state. */\n}\n`;
  edit('src/kosmickrisp/bridge/mtl_compiler.h', s => s.replace(
    '#endif /* MTL_COMPILER_H */',
    'void mtl_render_pipeline_descriptor_copy_linking(\n   mtl_render_pipeline_descriptor *copy, mtl_render_pipeline_descriptor *source);\n\n#endif /* MTL_COMPILER_H */'));
  for (const file of ['src/kosmickrisp/bridge/mtl_compiler.m', 'src/kosmickrisp/bridge/stubs/mtl_compiler.c'])
    edit(file, s => s + '\n' + linking);
  edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
    s = s.replace('#include "kk_unused_attachments.h"', '#include "kk_dgc.h"\n#include "kk_unused_attachments.h"\n#include "kk_dgc_pipeline.h"');
    s = s.replace('   kk_attachment_pipelines_finish(shader);',
      '   mtl_release(shader->dgc_compute);\n   kk_attachment_pipelines_finish(shader);');
    const retention = /if \(render && ([\s\S]*?)\) \{/;
    s = s.replace(retention, (_match, predicate) =>
      'if (render && (' + predicate + ' ||\n       device->vk.enabled_features.deviceGeneratedCommands ||\n       device->vk.enabled_features.drawIndirectCount)) {');
    s = s.replace('static VkResult\nkk_compile_compute_pipeline(struct kk_device *device,',
      'static VkResult\nkk_compile_compute_pipeline_mode(struct kk_device *device,');
    s = s.replace('                            mtl_compute_pipeline_state **pipe)\n{',
      '                            mtl_compute_pipeline_state **pipe, bool indirect)\n{');
    s = s.replace('   mtl_compute_pipeline_state *pipeline = mtl_new_compute_pipeline_state(\n      device->mtl_compiler_handle, function, local_size_threads);',
      `   mtl_compute_pipeline_state *pipeline = indirect
      ? kk_dgc_new_compute_pipeline_state(device->mtl_compiler_handle, function,
                                           local_size_threads)
      : mtl_new_compute_pipeline_state(device->mtl_compiler_handle, function,
                                        local_size_threads);`);
    s = s.replace('static mtl_depth_stencil_state *\nkk_compile_ds_state(',
      `static VkResult
kk_compile_compute_pipeline(struct kk_device *device,
                            const struct msl_compile_data *data,
                            uint32_t threads, mtl_compute_pipeline_state **pipe)
{
   return kk_compile_compute_pipeline_mode(device, data, threads, pipe, false);
}

VkResult
kk_compile_indirect_compute_pipeline(struct kk_device *device,
   const struct msl_compile_data *data, uint32_t threads,
   mtl_compute_pipeline_state **pipe)
{
   return kk_compile_compute_pipeline_mode(device, data, threads, pipe, true);
}

static mtl_depth_stencil_state *
kk_compile_ds_state(`);
    s = s.replace('   nir_shader *nir_shaders[shader_count + 2u];',
      '   bool dgc_replay = false;\n   nir_shader *nir_shaders[shader_count + 2u];');
    s = s.replace('      nir_shader *nir = info->nir;',
      '      nir_shader *nir = info->nir;\n      dgc_replay |= kk_dgc_shader_needs_replay(nir);');
    s = s.replace('   uint32_t push_size = 0;',
      `   for (uint32_t i = 0; i < total_shaders; ++i)
      dgc_replay |= shaders[i]->info.stage == MESA_SHADER_FRAGMENT &&
                    shaders[i]->info.fs.emulates_per_vertex_inputs;
   for (uint32_t i = 0; i < total_shaders; ++i) {
      shaders[i]->info.dgc_replay = dgc_replay;
      shaders[i]->info.indirect_bindable =
         infos[0].flags & VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT;
   }
   uint32_t push_size = 0;`);
    s = s.replace('   if (result != VK_SUCCESS || !vs->info.polygon_point) return result;',
      '   if (result != VK_SUCCESS || !vs->info.polygon_point)\n      return result == VK_SUCCESS ? kk_dgc_eager_graphics_pipeline(device, vs) : result;');
    s = s.replace('   return kk_compile_graphics_pipeline_native(device, vs, true);',
      '   result = kk_compile_graphics_pipeline_native(device, vs, true);\n   return result == VK_SUCCESS ? kk_dgc_eager_graphics_pipeline(device, vs) : result;');
    s = s.replace('                                     local_size_threads, &s->pipeline.cs);',
      '                                     local_size_threads, &s->pipeline.cs);\n      if (result == VK_SUCCESS && s->info.indirect_bindable &&\n          !kk_dgc_get_compute_pipeline(dev, s))\n         result = VK_ERROR_INVALID_SHADER_NV;');
    s = s.replace('         &shader->pipeline.cs);',
      '         &shader->pipeline.cs);\n      if (result == VK_SUCCESS && shader->info.indirect_bindable &&\n          !kk_dgc_get_compute_pipeline(dev, shader))\n         result = VK_ERROR_INVALID_SHADER_NV;');
    return s;
  });
  edit('src/kosmickrisp/vulkan/kk_unused_attachments.h', s => {
    s = s.replace('   uint32_t samples;', '   uint32_t samples;\n   bool indirect;');
    const signature = `mtl_render_pipeline_state *
kk_attachment_pipeline(struct kk_cmd_buffer *cmd, struct kk_shader *shader,
                       bool polygon)`;
    const begin = s.indexOf(signature), end = s.indexOf('\n#endif', begin);
    if (begin < 0 || end < 0) throw Error('Missing DGC attachment function anchor');
    const original = s.slice(begin, end);
    const formatSetup = `   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   enum mtl_pixel_format formats[MAX_DRAW_BUFFERS] = {0};
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      unsigned location = render->color_map[i];
      if (location < MAX_DRAW_BUFFERS && render->color_att[i].iview)
         formats[location] = vk_format_to_mtl_pixel_format(render->color_att[i].vk_format);
   }
`;
    let helper = checked(original, signature,
      `static mtl_render_pipeline_state *
kk_attachment_pipeline_for_formats(struct kk_device *device,
   struct kk_shader *shader, bool polygon, const enum mtl_pixel_format *formats,
   bool indirect, VkResult *error, uint32_t samples)`);
    helper = checked(helper, formatSetup, '');
    helper = checked(helper,
      '   uint32_t samples = MAX2(cmd->vk.dynamic_graphics_state.ms.rasterization_samples, 1);\n', '');
    helper = checked(helper, '   if (!cache)\n      return original;',
      '   if (!cache) {\n      if (indirect) *error = VK_ERROR_OUT_OF_HOST_MEMORY;\n      return indirect ? NULL : original;\n   }');
    helper = checked(helper, '   if (samples == shader->info.vs.sample_count &&',
      '   if (!indirect && samples == shader->info.vs.sample_count &&');
    helper = checked(helper, 'v->polygon == polygon &&',
      'v->polygon == polygon && v->indirect == indirect &&');
    helper = checked(helper, '      variant->pipeline = mtl_new_render_pipeline_for_attachments(',
      `      mtl_render_pipeline_descriptor *descriptor = indirect
         ? kk_dgc_render_descriptor(cache->descriptor[polygon])
         : mtl_retain(cache->descriptor[polygon]);
      if (!descriptor) {
         free(variant);
         simple_mtx_unlock(&cache->mutex);
         *error = VK_ERROR_OUT_OF_HOST_MEMORY;
         return NULL;
      }
      variant->pipeline = mtl_new_render_pipeline_for_attachments(`);
    helper = checked(helper, 'kk_cmd_buffer_device(cmd)->mtl_compiler_handle,', 'device->mtl_compiler_handle,');
    helper = checked(helper, 'cache->descriptor[polygon], formats,', 'descriptor, formats,');
    helper = checked(helper, '      result = VK_ERROR_INVALID_SHADER_NV;',
      '      mtl_release(descriptor);\n      result = VK_ERROR_INVALID_SHADER_NV;');
    helper = checked(helper, '         variant->polygon = polygon;',
      '         variant->polygon = polygon;\n         variant->indirect = indirect;');
    helper = checked(helper, '   vk_command_buffer_set_error(&cmd->vk, result);', '   *error = result;');
    if (!helper.includes('sizeof(formats)')) throw Error('Missing DGC format extent anchor');
    helper = helper.replaceAll('sizeof(formats)', 'sizeof(shader->info.vs.rt_formats)');
    const wrapper = `
mtl_render_pipeline_state *
kk_attachment_pipeline(struct kk_cmd_buffer *cmd, struct kk_shader *shader,
                       bool polygon)
{
${formatSetup}   uint32_t samples = MAX2(cmd->vk.dynamic_graphics_state.ms.rasterization_samples, 1);
   VkResult error = VK_SUCCESS;
   mtl_render_pipeline_state *pipeline = kk_attachment_pipeline_for_formats(
      kk_cmd_buffer_device(cmd), shader, polygon, formats,
      cmd->state.gfx.dgc_icb, &error, samples);
   if (!pipeline)
      vk_command_buffer_set_error(&cmd->vk, error);
   return pipeline;
}

VkResult
kk_dgc_eager_graphics_pipeline(struct kk_device *device, struct kk_shader *shader)
{
   if (!shader->info.indirect_bindable || shader->info.dgc_replay ||
       shader->info.vs.dynamic_vi)
      return VK_SUCCESS;
   for (unsigned polygon = 0; polygon < 2; ++polygon) {
      struct kk_attachment_pipelines *cache = shader->attachment_pipelines;
      if (!cache || !cache->descriptor[polygon]) continue;
      VkResult error = VK_SUCCESS;
      if (!kk_attachment_pipeline_for_formats(device, shader, polygon,
             shader->info.vs.rt_formats, true, &error, shader->info.vs.sample_count))
         return error;
   }
   return VK_SUCCESS;
}
`;
    return s.replace(original, helper + wrapper);
  });
  edit('src/kosmickrisp/vulkan/kk_vertex_input.h', s => {
    s = s.replace('kk_flush_vertex_input(struct kk_cmd_buffer *cmd)',
      'kk_prepare_vertex_input(struct kk_cmd_buffer *cmd,\n   const struct kk_graphics_state *gfx,\n   const struct vk_dynamic_graphics_state *dyn, const struct kk_shader *vs,\n   struct kk_root_descriptor_table *root)');
    s = s.replace('   struct kk_graphics_state *gfx = &cmd->state.gfx;\n   struct kk_descriptor_state *desc = &gfx->descriptors;\n   struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;\n   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];\n', '');
    s = s.replaceAll('desc->root.draw.', 'root->draw.');
    s = s.replace('   desc->root_dirty = true;\n', '');
    return s + `
static bool kk_flush_vertex_input(struct kk_cmd_buffer *cmd)
{
   bool success = kk_prepare_vertex_input(cmd, &cmd->state.gfx,
      &cmd->vk.dynamic_graphics_state, cmd->state.shaders[MESA_SHADER_VERTEX],
      &cmd->state.gfx.descriptors.root);
   cmd->state.gfx.descriptors.root_dirty = true;
   return success;
}
`;
  });
  edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => s.replace(
    'void kk_dgc_flush_graphics(struct kk_cmd_buffer *cmd) { kk_flush_gfx_state(cmd); }',
    `void kk_dgc_flush_graphics(struct kk_cmd_buffer *cmd) { kk_flush_gfx_state(cmd); }
bool kk_dgc_prepare_graphics_root(struct kk_cmd_buffer *allocator,
   struct kk_cmd_buffer *state, struct kk_root_descriptor_table *root)
{
   return kk_prepare_vertex_input(allocator, &state->state.gfx,
      &state->vk.dynamic_graphics_state, state->state.shaders[MESA_SHADER_VERTEX], root);
}`));

}
