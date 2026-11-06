// SPDX-License-Identifier: MIT
export function apply({edit}) {
// Mesa owns GS lowering, main/raster MSL, dispatch, topology and caching.
// Keep only the auxiliary Poly passes needed by transform feedback and queries.
edit('src/kosmickrisp/vulkan/kk_shader.h', s => s
 .replace('   bool uses_per_draw_data;', `   bool uses_per_draw_data;
   bool has_geometry;
   struct poly_gs_info geometry;
   uint8_t gs_aux_mask;`)
 .replace('         mtl_compute_pipeline_state *pre_render[4];', `         mtl_compute_pipeline_state *pre_render[4];
         mtl_compute_pipeline_state *gs_aux[2];`)
 .replace('         bool uses_flat_varyings;', '         bool uses_flat_varyings;\n         uint64_t flat_outputs;')
 .replace('   struct msl_compile_data gs_main;', `   struct msl_compile_data gs_main;
   struct msl_compile_data gs_aux[2];`));

edit('src/kosmickrisp/vulkan/kk_shader.c', s => {
  s = s.replace('static bool\nfs_uses_flat_varying(nir_shader *nir)', 'static uint64_t\nfs_flat_output_mask(nir_shader *nir)');
  const maskStart = s.indexOf('fs_flat_output_mask(nir_shader *nir)');
  const maskEnd = s.indexOf('static void\ngather_shader_info',maskStart);
  let mask = s.slice(maskStart,maskEnd);
  mask = mask.replace('{\n   nir_foreach_function_impl', '{\n   uint64_t result = 0;\n   nir_foreach_function_impl')
   .replace('                  return true;', '                  result |= BITFIELD64_BIT(nir_intrinsic_io_semantics(intr).location);')
   .replace('   return false;', '   return result;');
  s = s.slice(0,maskStart) + mask + s.slice(maskEnd);
  s = s.replace('      shader->info.fs.uses_flat_varyings = fs_uses_flat_varying(nir);',
    '      shader->info.fs.flat_outputs = fs_flat_output_mask(nir);\n      shader->info.fs.uses_flat_varyings = shader->info.fs.flat_outputs != 0;');

 s=s.replace('#include "poly/nir/poly_nir.h"', '#include "poly/nir/poly_nir.h"\n#include "poly/geometry.h"');
 s=s.replace(`      /* Only transform feedback and pipeline statistics need these, and KK
       * exposes neither */
      ralloc_free(count);
      ralloc_free(pre_gs);`, `      shader->info.has_geometry = true;
      shader->info.geometry = shader->info.gs;
      nir_shader *aux[] = {count, pre_gs};
      for (unsigned i = 0; i < ARRAY_SIZE(aux); ++i) {
         if (!aux[i]) continue;
         shader->info.gs_aux_mask |= BITFIELD_BIT(i);
         aux[i]->info.stage = MESA_SHADER_COMPUTE;
         memset(&aux[i]->info.cs, 0, sizeof(aux[i]->info.cs));
         aux[i]->info.workgroup_size[0] = i ? 1 : KK_POLY_WG_SIZE;
         aux[i]->info.workgroup_size[1] = aux[i]->info.workgroup_size[2] = 1;
         msl_preprocess_nir(aux[i]);
         kk_compile_msl(pdev, aux[i], state, &shader->gs_aux[i]);
      }`);
 s=s.replace('         kk_msl_copy(&vs->gs_main, &s->gs_main);', `         kk_msl_copy(&vs->gs_main, &s->gs_main);
         info->has_geometry = true;
         info->geometry = s->info.gs;
         info->gs_aux_mask = s->info.gs_aux_mask;
         u_foreach_bit(i, info->gs_aux_mask)
            kk_msl_copy(&vs->gs_aux[i], &s->gs_aux[i]);`);
 s=s.replace('   const struct msl_compile_data *vs_data = &vs->msl_data[vs_stage];', `   u_foreach_bit(i, vs->info.gs_aux_mask) {
      result = kk_compile_compute_pipeline(device, &vs->gs_aux[i],
         i ? 1 : KK_POLY_WG_SIZE, &pipe->gfx.gs_aux[i]);
      if (result != VK_SUCCESS) return result;
   }
   const struct msl_compile_data *vs_data = &vs->msl_data[vs_stage];`);
 s=s.replace('   ralloc_free((void *)shader->gs_main.entrypoint_name);', `   u_foreach_bit(i, shader->info.gs_aux_mask) {
      if (shader->info.stage == MESA_SHADER_VERTEX)
         mtl_release(pipe->gfx.gs_aux[i]);
      ralloc_free(shader->gs_aux[i].entrypoint_name);
      ralloc_free(shader->gs_aux[i].code);
   }
   ralloc_free((void *)shader->gs_main.entrypoint_name);`);
 s=s.replace('   return !blob->out_of_memory;', `   u_foreach_bit(i, shader->info.gs_aux_mask)
      kk_msl_serialize(&shader->gs_aux[i], blob);
   return !blob->out_of_memory;`);
 s=s.replace('   if (info.stage == MESA_SHADER_COMPUTE) {', `   u_foreach_bit(i, info.gs_aux_mask) {
      result = kk_msl_deserialize(blob, &shader->gs_aux[i]);
      if (result != VK_SUCCESS) goto fail;
   }
   if (info.stage == MESA_SHADER_COMPUTE) {`);
 s=s.replace('      msl_preprocess_nir_workarounds(nir, pdev->settings.disabled_workarounds);', `      /* XFB without an explicit GS gets a Poly passthrough after linking.
       * Keep its producer in compute form before hardware-VS lowering. */
      emulated_stage |= nir->xfb_info && (nir->info.stage == MESA_SHADER_VERTEX ||
                                         nir->info.stage == MESA_SHADER_TESS_EVAL);
      msl_preprocess_nir_workarounds(nir, pdev->settings.disabled_workarounds);`);
 s=s.replace('nir_shader *nir_shaders[shader_count + 1u];', 'nir_shader *nir_shaders[shader_count + 2u];');
 s=s.replace('struct kk_shader *shaders[shader_count + 1u];', 'struct kk_shader *shaders[shader_count + 2u];');
 s=s.replace('   uint32_t total_shaders = shader_count;', `   bool explicit_geometry = false, xfb = false, tess = false;
   for (unsigned i = 0; i < shader_count; ++i) {
      explicit_geometry |= infos[i].stage == MESA_SHADER_GEOMETRY;
      tess |= infos[i].stage == MESA_SHADER_TESS_EVAL;
      xfb |= nir_shaders[i]->xfb_info != NULL;
   }
   uint32_t total_shaders = shader_count;`);
  const emptyOriginal = `   uint32_t total_shaders = shader_count;
   if (state && infos[shader_count - 1u].stage != MESA_SHADER_FRAGMENT) {
      nir_shaders[shader_count] =
         get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);
      total_shaders += 1u;
   }`;
  s = s.replace(emptyOriginal, `   uint32_t total_shaders = shader_count;
   if (!explicit_geometry && xfb) {
      nir_shader *producer = NULL;
      unsigned insert = shader_count;
      for (unsigned i = 0; i < shader_count; ++i) {
         if (nir_shaders[i]->xfb_info) producer = nir_shaders[i];
         if (nir_shaders[i]->info.stage == MESA_SHADER_FRAGMENT) insert = i;
      }
      assert(producer && state);
      producer->info.next_stage = MESA_SHADER_GEOMETRY;
      size_t key_size = poly_passthrough_gs_key_size(producer->xfb_info->output_count);
      struct poly_passthrough_gs_key *key = rzalloc_size(NULL, key_size);
      key->prim = u_decomposed_prim(vk_topology_to_mesa(state->ia->primitive_topology));
      if (tess) {
         key->prim = producer->info.tess.point_mode ? MESA_PRIM_POINTS :
            (producer->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES ? MESA_PRIM_LINES : MESA_PRIM_TRIANGLES);
      }
      key->outputs = producer->info.outputs_written;
      key->clip_distance_array_size = producer->info.clip_distance_array_size;
      key->cull_distance_array_size = producer->info.cull_distance_array_size;
      memcpy(key->xfb_stride, producer->info.xfb_stride, sizeof(key->xfb_stride));
      memcpy(&key->xfb_info, producer->xfb_info,
             nir_xfb_info_size(producer->xfb_info->output_count));
      nir_foreach_function_impl(impl, producer) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic) continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if (intr->intrinsic != nir_intrinsic_store_output) continue;
               unsigned slot = nir_intrinsic_io_semantics(intr).location;
               key->output_components[slot] = MAX2(key->output_components[slot],
                  nir_intrinsic_component(intr) + intr->num_components);
               key->output_types[slot] = nir_intrinsic_src_type(intr);
            }
         }
      }
      nir_builder builder = nir_builder_init_simple_shader(MESA_SHADER_GEOMETRY,
         producer->options, "Poly transform feedback passthrough");
      poly_nir_passthrough_gs(&builder, key);
      ralloc_free(key);
      builder.shader->info.io_lowered = true;
      msl_preprocess_nir(builder.shader);
      for (unsigned i = total_shaders; i > insert; --i)
         nir_shaders[i] = nir_shaders[i - 1];
      nir_shaders[insert] = builder.shader;
      ++total_shaders;
   }
   if (state && nir_shaders[total_shaders - 1]->info.stage != MESA_SHADER_FRAGMENT) {
      nir_shaders[total_shaders] = get_empty_nir(dev, MESA_SHADER_FRAGMENT, state, features);
      ++total_shaders;
   }`);
  s = s.replace('   nir_opt_varyings_bulk(nir_shaders, total_shaders, true, UINT32_MAX,', `   /* Preserve upstream XFB side effects through the original varying linker,
    * before Poly lowers those IO stores to Metal device writes. */
   for (unsigned i = 0; i < total_shaders; ++i) {
      if (nir_shaders[i]->xfb_info)
         NIR_PASS(_, nir_shaders[i], nir_io_add_intrinsic_xfb_info);
   }
   nir_opt_varyings_bulk(nir_shaders, total_shaders, true, UINT32_MAX,`);
  s = s.replace(`      /* Free generated fragment shader */
      if (shader_count != total_shaders)
         kk_shader_destroy(&dev->vk, shaders[shader_count], pAllocator);`, `      /* Generated Poly and empty-fragment leaves are already copied into the
       * original vertex pipeline and share its normal lifetime. */`);
  s = s.replace('      for (uint32_t i = 0; i < shader_count; i++) {\n         kk_shader_destroy(&dev->vk, shaders[i], pAllocator);', '      for (uint32_t i = 0; i < total_shaders; i++) {\n         kk_shader_destroy(&dev->vk, shaders[i], pAllocator);');
  s = s.replace('      memcpy(shaders_out, shaders, shader_count * sizeof(*shaders_out));', `      for (unsigned i = 0; i < shader_count; ++i) {
         for (unsigned j = 0; j < total_shaders; ++j)
            if (shaders[j]->vk.stage == infos[i].stage) shaders_out[i] = &shaders[j]->vk;
      }
      for (unsigned j = 0; j < total_shaders; ++j) {
         bool original = false;
         for (unsigned i = 0; i < shader_count; ++i) original |= shaders[j]->vk.stage == infos[i].stage;
         if (!original) kk_shader_destroy(&dev->vk, shaders[j], pAllocator);
      }`);
  return s;
});

edit('src/kosmickrisp/vulkan/kk_cmd_buffer.h', s => s
 .replace('#define KK_CMD_BUFFER_H 1', '#define KK_CMD_BUFFER_H 1\n#include "gallium/include/pipe/p_defines.h"\n#include "poly/geometry.h"')
 .replace('   uint64_t geometry_params;', `   uint64_t geometry_params;
   uint64_t geometry_statistics[PIPE_STAT_QUERY_COUNT];`)
 .replace('   struct kk_per_draw_data per_draw_data;', `   struct kk_per_draw_data per_draw_data;
   struct kk_addr_range xfb[POLY_MAX_SO_BUFFERS];
   uint64_t xfb_offsets;
   uint64_t xfb_query[POLY_MAX_VERTEX_STREAMS];
   uint64_t primitives_query[POLY_MAX_VERTEX_STREAMS];
   bool xfb_enabled;`));

edit('src/kosmickrisp/vulkan/kk_nir_lower_descriptors.c', s => s.replace('   case nir_intrinsic_load_tess_param_buffer_poly:', `   case nir_intrinsic_ro_to_rw_poly:
      nir_def_replace(&intrin->def, intrin->src[0].ssa);
      return true;
   case nir_intrinsic_load_stat_query_address_poly: {
      b->cursor = nir_before_instr(&intrin->instr);
      unsigned offset = kk_per_draw_offset(geometry_statistics) +
                        nir_intrinsic_base(intrin) * sizeof(uint64_t);
      nir_def_replace(&intrin->def, load_per_draw(b, 1, 64,
                         nir_imm_int(b, offset), 8));
      return true;
   }
   case nir_intrinsic_load_tess_param_buffer_poly:`));

edit('src/kosmickrisp/libkk/kk_geometry.cl', s => s
 .replace('uint32_t prim, uint32_t max_indices,', 'uint32_t prim, int is_prefix_summing, uint32_t max_indices,')
 .replace('   /* Prefix summing only serves transform feedback */\n', '')
 .replace('false /* is_prefix_summing */', 'is_prefix_summing') + `
KERNEL(1024)
libkk_prefix_sum_geometry(global struct poly_geometry_params *p)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->count_buffer, p->input_primitives,
      p->count_buffer_stride / 4, cl_group_id.x, 1024);
}

KERNEL(1)
libkk_xfb_counter_copy(uint64_t source, uint64_t destination)
{
   *(global uint32_t *)destination = source ? *(global uint32_t *)source : 0;
}

KERNEL(1)
libkk_xfb_draw(constant uint32_t *counter, global uint32_t *draw,
              uint32_t instance_count, uint32_t first_instance,
              uint32_t counter_offset, uint32_t vertex_stride)
{
   draw[0] = max((int)*counter - (int)counter_offset, 0) / vertex_stride;
   draw[1] = instance_count;
   draw[2] = 0;
   draw[3] = first_instance;
}
`);

edit('src/kosmickrisp/vulkan/kk_cmd_draw.c', s => {
 s=s.replaceAll('cmd->state.shaders[MESA_SHADER_GEOMETRY]', 'kk_geometry_shader(cmd)');
 s=s.replaceAll('gs->info.gs', 'gs->info.geometry');
 const anchor='static enum mesa_prim\nkk_rast_prim';
 s=s.replace(anchor, `/* Synthetic XFB passthrough stages are retained by the VS pipeline. */
static struct kk_shader *
kk_geometry_shader(struct kk_cmd_buffer *cmd)
{
   struct kk_shader *gs = cmd->state.shaders[MESA_SHADER_GEOMETRY];
   struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
   return gs ? gs : vs && vs->info.has_geometry ? vs : NULL;
}

`+anchor);
 s=s.replace('   poly_geometry_params_init(&params, mode, wg_size);', `   poly_geometry_params_init(&params, mode, wg_size);
   const struct poly_gs_info *info = &gs->info.geometry;
   params.count_buffer_stride = info->count_words * sizeof(uint32_t);
   struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   params.flat_outputs = fs ? fs->info.fs.flat_outputs : 0;
   uint64_t scratch = kk_pool_upload(cmd, (uint64_t[16]){0}, 128, 8).gpu;
   for (unsigned i = 0; i < POLY_MAX_VERTEX_STREAMS; ++i) {
      params.prims_generated_counter[i] = scratch + 8 * i;
      params.xfb_prims_generated_counter[i] = scratch + 32 + 8 * i;
      params.xfb_overflow[i] = scratch + 64 + 4 * i;
      params.xfb_base_original[i] = scratch;
      params.xfb_offs_ptrs[i] = scratch + 80 + 4 * i;
      if (gfx->primitives_query[i])
         params.prims_generated_counter[i] = gfx->primitives_query[i];
      if (gfx->xfb_query[i]) {
         params.xfb_prims_generated_counter[i] = gfx->xfb_query[i];
         params.prims_generated_counter[i] = gfx->xfb_query[i] + sizeof(uint64_t);
      }
      if (gfx->xfb_enabled) {
         params.xfb_base_original[i] = gfx->xfb[i].addr;
         params.xfb_size[i] = gfx->xfb[i].range;
         params.xfb_offs_ptrs[i] = gfx->xfb_offsets + i * sizeof(uint32_t);
      }
   }
   params.xfb_any_overflow = scratch + 96;
   for (unsigned i = 0; i < PIPE_STAT_QUERY_COUNT; ++i)
      gfx->per_draw_data.geometry_statistics[i] = scratch + 104;
   if (info->count_words && !info->prefix_sum)
      params.count_buffer = scratch + 112;`);
 s=s.replace('      gfx->gs.index_count = params.draw.index_count;', `      if (info->prefix_sum)
         params.count_buffer = kk_pool_alloc(cmd,
            params.input_primitives * params.count_buffer_stride, 4).gpu;
      gfx->gs.index_count = params.draw.index_count;`);
 s=s.replace('         .max_indices = gs->info.geometry.max_indices,', `         .is_prefix_summing = gs->info.geometry.prefix_sum,
         .max_indices = gs->info.geometry.max_indices,`);
 const beforeMain=`   mtl_barrier_after_encoder_stages(enc, MTL_STAGE_DISPATCH,
                                    MTL_STAGE_DISPATCH);
   mtl_compute_set_pipeline_state(
      enc, vs->pipeline.gfx.pre_render[pre_render_count - 1u]);`;
 s=s.replace(beforeMain, `   bool queries = false;
   for (unsigned i = 0; i < POLY_MAX_VERTEX_STREAMS; ++i)
      queries |= gfx->xfb_query[i] || gfx->primitives_query[i];
   if (gs->info.geometry.xfb || queries) {
      if (vs->info.gs_aux_mask & BITFIELD_BIT(0)) {
         mtl_barrier_after_encoder_stages(enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
         mtl_compute_set_pipeline_state(enc, vs->pipeline.gfx.gs_aux[0]);
         kk_dispatch_compute(enc, grid_gs, local_size);
      }
      if (gs->info.geometry.prefix_sum)
         libkk_prefix_sum_geometry(cmd, kk_grid_1d(1024 * gs->info.geometry.count_words), true, geometry_params);
      enc = cs_get_compute(cmd);
      if (vs->info.gs_aux_mask & BITFIELD_BIT(1)) {
         mtl_barrier_after_encoder_stages(enc, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
         mtl_compute_set_pipeline_state(enc, vs->pipeline.gfx.gs_aux[1]);
         kk_dispatch_compute(enc, kk_grid_1d(1), (struct mtl_size){1, 1, 1});
      }
   }
`+beforeMain);
  s += `
VKAPI_ATTR void VKAPI_CALL
kk_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer,
   uint32_t firstBinding, uint32_t bindingCount, const VkBuffer *pBuffers,
   const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   for (unsigned i = 0; i < bindingCount; ++i) {
      VK_FROM_HANDLE(kk_buffer, buffer, pBuffers[i]);
      cmd->state.gfx.xfb[firstBinding + i] = kk_buffer_addr_range(buffer,
         pOffsets[i], pSizes ? pSizes[i] : VK_WHOLE_SIZE);
   }
}

static void
kk_begin_end_xfb(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets, bool begin)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   gfx->xfb_enabled = begin;
   if (!gfx->xfb_offsets)
      gfx->xfb_offsets = kk_pool_alloc(cmd, POLY_MAX_SO_BUFFERS * 4, 4).gpu;
   for (unsigned i = 0; i < (begin ? POLY_MAX_SO_BUFFERS : count); ++i) {
      unsigned slot = begin ? i : first + i;
      bool selected = slot >= first && slot - first < count && buffers &&
                      buffers[slot - first] != VK_NULL_HANDLE;
      uint64_t source = 0, destination = gfx->xfb_offsets + slot * 4;
      if (selected) {
         VK_FROM_HANDLE(kk_buffer, buffer, buffers[slot - first]);
         source = vk_buffer_address(&buffer->vk, offsets ? offsets[slot - first] : 0);
      }
      if (!begin) {
         if (!selected) continue;
         uint64_t tmp = destination; destination = source; source = tmp;
      }
      libkk_xfb_counter_copy(cmd, kk_grid_1d(1), true, source, destination);
   }
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)
{
   kk_begin_end_xfb(commandBuffer, first, count, buffers, offsets, true);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t first,
   uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)
{
   kk_begin_end_xfb(commandBuffer, first, count, buffers, offsets, false);
}

VKAPI_ATTR void VKAPI_CALL
kk_CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer,
   uint32_t instanceCount, uint32_t firstInstance, VkBuffer counterBuffer,
   VkDeviceSize counterBufferOffset, uint32_t counterOffset, uint32_t vertexStride)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(kk_buffer, counter, counterBuffer);
   uint64_t draw = kk_pool_alloc(cmd, sizeof(VkDrawIndirectCommand), 4).gpu;
   libkk_xfb_draw(cmd, kk_grid_1d(1), true,
      vk_buffer_address(&counter->vk, counterBufferOffset), draw,
      instanceCount, firstInstance, counterOffset, vertexStride);
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   struct kk_draw_command data = {
      .prim = vk_topology_to_mesa(dyn->ia.primitive_topology),
      .upload_mask = build_per_draw_upload_mask(cmd),
      .draw_count = 1, .indirect = true,
      .indirect_command = { .addr = draw, .stride = sizeof(VkDrawIndirectCommand) },
   };
   kk_draw(cmd, &data);
}
`;
  return s;
});

edit('src/kosmickrisp/vulkan/kk_query_pool.c', s => {
  s = s.replace('   case VK_QUERY_TYPE_TIMESTAMP:\n      return 1;', `   case VK_QUERY_TYPE_TIMESTAMP:
   case VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT:
      return 1;
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
      return 2;`);
  s = s.replaceAll('   uint32_t index =\n      kk_pool_is_ts(pool) ? query : kk_pool_index_ptr(pool)[query];', `   uint32_t index = kk_pool_is_oq(pool) ? kk_pool_index_ptr(pool)[query]
                                                : query * kk_reports_per_query(pool);`);
  s = s.replaceAll('kk_pool_is_ts(pool) ? 0u : pool->bo->gpu + pool->index_start',
    'kk_pool_is_oq(pool) ? pool->bo->gpu + pool->index_start : 0u');
  s = s.replace(`VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                 uint32_t query, VkQueryControlFlags flags)`, `static void
kk_begin_query(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
               uint32_t query, VkQueryControlFlags flags, uint32_t index)`);
  s = s.replace('   cmd->state.gfx.occlusion.mode = flags & VK_QUERY_CONTROL_PRECISE_BIT', `   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {
      cmd->state.gfx.xfb_query[index] = kk_query_report_addr(kk_cmd_buffer_device(cmd), pool, query);
      return;
   }
   if (pool->vk.query_type == VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT) {
      cmd->state.gfx.primitives_query[index] = kk_query_report_addr(kk_cmd_buffer_device(cmd), pool, query);
      return;
   }
   cmd->state.gfx.occlusion.mode = flags & VK_QUERY_CONTROL_PRECISE_BIT`);
  s = s.replace(`VKAPI_ATTR void VKAPI_CALL
kk_CmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
               uint32_t query)`, `static void
kk_end_query(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
             uint32_t query, uint32_t index)`);
  s = s.replace(`   cmd->state.gfx.occlusion.mode = MTL_VISIBILITY_RESULT_MODE_DISABLED;
   cmd->state.gfx.dirty |= KK_DIRTY_OCCLUSION;`, `   if (pool->vk.query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT) {
      cmd->state.gfx.xfb_query[index] = 0;
   } else if (pool->vk.query_type == VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT) {
      cmd->state.gfx.primitives_query[index] = 0;
   } else {
      cmd->state.gfx.occlusion.mode = MTL_VISIBILITY_RESULT_MODE_DISABLED;
      cmd->state.gfx.dirty |= KK_DIRTY_OCCLUSION;
   }`);
  s += `
VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
   uint32_t query, VkQueryControlFlags flags)
{
   kk_begin_query(commandBuffer, queryPool, query, flags, 0);
}
VKAPI_ATTR void VKAPI_CALL
kk_CmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{
   kk_end_query(commandBuffer, queryPool, query, 0);
}
VKAPI_ATTR void VKAPI_CALL
kk_CmdBeginQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
   uint32_t query, VkQueryControlFlags flags, uint32_t index)
{
   kk_begin_query(commandBuffer, queryPool, query, flags, index);
}
VKAPI_ATTR void VKAPI_CALL
kk_CmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
   uint32_t query, uint32_t index)
{
   kk_end_query(commandBuffer, queryPool, query, index);
}
`;
  return s;
});

edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s
 .replace('      /* Optional extensions */', `      .EXT_transform_feedback = true,
      .EXT_primitives_generated_query = true,
      /* Optional extensions */`)
 .replace('      .tessellationShader = true,', `      .tessellationShader = true,
      .transformFeedback = true,
      .geometryStreams = false,
      .primitivesGeneratedQuery = true,
      .primitivesGeneratedQueryWithRasterizerDiscard = true,
      .primitivesGeneratedQueryWithNonZeroStreams = false,`)
 .replace('      .transformFeedbackRasterizationStreamSelect = true,', '      .transformFeedbackRasterizationStreamSelect = false,'));
}
