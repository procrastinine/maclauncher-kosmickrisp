/* SPDX-License-Identifier: MIT */
#include "kk_bo.h"
#include "kk_cmd_buffer.h"
#include "kk_device.h"
#include "kk_dgc.h"
#include "kk_entrypoints.h"
#include "kk_physical_device.h"
#include "kk_shader.h"
#include "kk_nir_lower_vbo.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/vk_to_mtl_map.h"
#include "vk_alloc.h"
#include "vk_device_generated_commands.h"
#include "vk_format.h"

struct kk_indirect_commands_layout {
   struct vk_indirect_command_layout vk;
   uint32_t token_span;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(kk_indirect_commands_layout, vk.base,
                               VkIndirectCommandsLayoutEXT,
                               VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT)

#define ROOT_SIZE sizeof(struct kk_root_descriptor_table)
#define DRAW_OFFSET ALIGN_POT(ROOT_SIZE, 8)
#define ARGS_OFFSET ALIGN_POT(DRAW_OFFSET + sizeof(struct kk_per_draw_data), 8)
#define INDEX_OFFSET ALIGN_POT(ARGS_OFFSET + 20, 8)
#define RECORD_STRIDE ALIGN_POT(INDEX_OFFSET + 16, 64)
#define SNAPSHOT_COUNT 0u
#define SNAPSHOT_HEADER 64u

static uint32_t kk_dgc_token_span(const struct vk_indirect_command_layout *l)
{
   uint32_t size =
       (l->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DISPATCH))
           ? l->dispatch_src_offset_B + 12
           : l->draw_src_offset_B +
                 ((l->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED)) ? 20
                                                                         : 16);
   for (uint32_t i = 0; i < l->n_pc_layouts; ++i)
      size =
          MAX2(size, l->pc_layouts[i].src_offset_B + l->pc_layouts[i].size_B);
   for (uint32_t i = 0; i < l->n_vb_layouts; ++i)
      size = MAX2(size, l->vb_layouts[i].src_offset_B + 16);
   if (l->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB))
      size = MAX2(size, l->index_src_offset_B + 16);
   return ALIGN_POT(size, 4);
}
#define KK_DGC_ICB_SLICE 16384u

VKAPI_ATTR VkResult VKAPI_CALL kk_CreateIndirectCommandsLayoutEXT(
    VkDevice device, const VkIndirectCommandsLayoutCreateInfoEXT *info,
    const VkAllocationCallbacks *alloc, VkIndirectCommandsLayoutEXT *out)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   struct kk_indirect_commands_layout *layout =
       vk_indirect_command_layout_create(&dev->vk, info, alloc,
                                         sizeof(*layout));
   if (!layout)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   layout->token_span = kk_dgc_token_span(&layout->vk);
   *out = kk_indirect_commands_layout_to_handle(layout);
   return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL kk_DestroyIndirectCommandsLayoutEXT(
    VkDevice device, VkIndirectCommandsLayoutEXT handle,
    const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(kk_device, dev, device);
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout, handle);
   if (layout)
      vk_indirect_command_layout_destroy(&dev->vk, alloc, &layout->vk);
}
VKAPI_ATTR void VKAPI_CALL kk_GetGeneratedCommandsMemoryRequirementsEXT(
    VkDevice device, const VkGeneratedCommandsMemoryRequirementsInfoEXT *info,
    VkMemoryRequirements2 *requirements)
{
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout,
                  info->indirectCommandsLayout);
   requirements->memoryRequirements = (VkMemoryRequirements){
       .size = SNAPSHOT_HEADER + (uint64_t)info->maxSequenceCount *
                                     layout->token_span,
       .alignment = 64,
       .memoryTypeBits = 1,
   };
}
/* Indirect execution sets are a separately reported optional facility. */
VKAPI_ATTR VkResult VKAPI_CALL kk_CreateIndirectExecutionSetEXT(
    VkDevice device, const VkIndirectExecutionSetCreateInfoEXT *info,
    const VkAllocationCallbacks *alloc, VkIndirectExecutionSetEXT *out)
{
   *out = VK_NULL_HANDLE;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}
VKAPI_ATTR void VKAPI_CALL kk_DestroyIndirectExecutionSetEXT(
    VkDevice device, VkIndirectExecutionSetEXT set,
    const VkAllocationCallbacks *alloc)
{
}

static struct kk_dgc_params
kk_dgc_parameters(struct kk_cmd_buffer *cmd,
                  const VkGeneratedCommandsInfoEXT *info)
{
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout,
                  info->indirectCommandsLayout);
   const struct vk_indirect_command_layout *l = &layout->vk;
   bool compute = l->stages & VK_SHADER_STAGE_COMPUTE_BIT;
   struct kk_descriptor_state *desc =
       compute ? &cmd->state.cs.descriptors : &cmd->state.gfx.descriptors;
   struct kk_dgc_params p = {
       .stream = info->indirectAddress,
       .count = info->sequenceCountAddress,
       .output = info->preprocessAddress,
       .maximum = info->maxSequenceCount,
       .stream_stride = l->stride,
       .record_stride = RECORD_STRIDE,
       .root_size = ROOT_SIZE,
       .draw_offset = DRAW_OFFSET,
       .args_offset = ARGS_OFFSET,
       .index_offset = INDEX_OFFSET,
       .action_offset =
           compute ? l->dispatch_src_offset_B : l->draw_src_offset_B,
       .index_token = (l->dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB))
                          ? l->index_src_offset_B
                          : UINT32_MAX,
       .root_push = offsetof(struct kk_root_descriptor_table, push),
       .root_vertex_input = offsetof(struct kk_root_descriptor_table, draw.vertex_input),
       .vertex_fetch_size = sizeof(struct kk_vertex_fetch),
       .vertex_fetch_stride = offsetof(struct kk_vertex_fetch, stride),
       .root_strides =
           offsetof(struct kk_root_descriptor_table, draw.buffer_strides),
       .root_bases =
           offsetof(struct kk_root_descriptor_table, draw.attrib_base),
       .root_clamps =
           offsetof(struct kk_root_descriptor_table, draw.attrib_clamps),
       .draw_base_vertex = offsetof(struct kk_per_draw_data, base_vertex_addr),
       .draw_base_instance =
           offsetof(struct kk_per_draw_data, base_instance_addr),
       .action =
           compute
               ? 2
               : ((l->dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED)) ? 1
                                                                         : 0),
       .index = cmd->state.gfx.index.gpu.addr,
       .index_size = cmd->state.gfx.index.bytes_per_index,
       .dx_index = l->index_mode_is_dx,
       .index_range = MIN2(cmd->state.gfx.index.gpu.range, UINT32_MAX),
       .samplers = kk_cmd_buffer_device(cmd)->samplers.table.bo->gpu,
       .condition =
           cmd->state.cond_render.enabled ? cmd->state.cond_render.address : 0,
       .inverted = cmd->state.cond_render.inverted,
   };
   if (!compute) {
      const struct kk_shader *vs = cmd->state.shaders[MESA_SHADER_VERTEX];
      p.vertex_input_size = vs->info.vs.dynamic_vi
         ? util_bitcount(vs->info.vs.attribs_read) * sizeof(struct kk_vertex_fetch) : 0;
   }
   if (compute) {
      const struct kk_shader *cs = cmd->state.shaders[MESA_SHADER_COMPUTE];
      p.local[0] = cs->info.cs.local_size.x;
      p.local[1] = cs->info.cs.local_size.y;
      p.local[2] = cs->info.cs.local_size.z;
   } else if (!kk_dgc_needs_poly(cmd))
      p.primitive = kk_dgc_primitive(cmd);
   return p;
}

static struct kk_dgc_params kk_dgc_state(struct kk_cmd_buffer *cmd,
                                         struct kk_cmd_buffer *state,
                                         const VkGeneratedCommandsInfoEXT *info)
{
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout,
                  info->indirectCommandsLayout);
   const struct vk_indirect_command_layout *l = &layout->vk;
   bool compute = l->stages & VK_SHADER_STAGE_COMPUTE_BIT;
   kk_sparse_refresh_shader(state, compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX);
   struct kk_dgc_params p = kk_dgc_parameters(state, info);
   struct kk_descriptor_state *desc =
       compute ? &state->state.cs.descriptors : &state->state.gfx.descriptors;
   struct kk_descriptor_state descriptors = *desc;
   if (descriptors.push_dirty)
      kk_cmd_buffer_flush_push_descriptors(cmd, &descriptors);
   struct kk_root_descriptor_table root = descriptors.root;
   root.cs.base_group[0] = root.cs.base_group[1] = root.cs.base_group[2] = 0;
   if (!compute) {
      root = descriptors.root;
      const struct vk_dynamic_graphics_state *dyn =
          &state->vk.dynamic_graphics_state;
      memcpy(root.draw.blend_constant, dyn->cb.blend_constants,
             sizeof(root.draw.blend_constant));
      root.draw.clip_z_coeff =
          dyn->vp.depth_clip_negative_one_to_one ? 0.5f : 0.0f;
      bool clamp = dyn->rs.depth_clamp_enable;
      bool clip = vk_rasterization_state_depth_clip_enable(&dyn->rs);
      root.draw.emulate_depth_clamp = clamp && clip;
      root.draw.emulate_viewport_z = !clamp && !clip;
      for (uint32_t i = 0; i < dyn->vp.viewport_count; ++i) {
         const VkViewport *vp = &dyn->vp.viewports[i];
         root.draw.viewport_z_range[i * 2] =
             root.draw.emulate_depth_clamp ? MIN2(vp->minDepth, vp->maxDepth)
                                           : vp->minDepth;
         root.draw.viewport_z_range[i * 2 + 1] =
             root.draw.emulate_depth_clamp ? MAX2(vp->minDepth, vp->maxDepth)
                                           : vp->maxDepth;
      }
      if (!kk_dgc_prepare_graphics_root(cmd, state, &root))
         return (struct kk_dgc_params){0};
   }
   kk_descriptor_root_apply_heap(&descriptors, &root);
   p.base_root = kk_upload_descriptor_root_data(cmd, &root, sizeof(root));
   struct kk_per_draw_data draw = state->state.gfx.per_draw_data;
   draw.draw_id = 0;
   p.base_draw = kk_pool_upload(cmd, &draw, sizeof(draw), 8).gpu;
   struct kk_dgc_patch patches[129];
   for (uint32_t i = 0; i < l->n_pc_layouts; ++i) {
      const struct vk_indirect_command_push_constant_layout *pc =
          &l->pc_layouts[i];
      patches[p.patch_count++] = (struct kk_dgc_patch){
          pc->src_offset_B, pc->dst_offset_B, pc->size_B, 0};
   }
   if (l->dgc_info & BITFIELD_BIT(MESA_VK_DGC_SI))
      patches[p.patch_count++] =
          (struct kk_dgc_patch){0, l->si_layout.dst_offset_B, 4, 1};
   if (p.patch_count)
      p.patches =
          kk_pool_upload(cmd, patches, p.patch_count * sizeof(patches[0]), 8)
              .gpu;
   struct kk_dgc_attribute attributes[KK_MAX_ATTRIBS];
   if (!compute) {
      const struct vk_dynamic_graphics_state *dyn =
          &state->vk.dynamic_graphics_state;
      struct kk_shader *vs = state->state.shaders[MESA_SHADER_VERTEX];
      uint32_t slot = 0;
      u_foreach_bit(a, vs->info.vs.attribs_read)
      {
         if (dyn->vi->attributes_valid & BITFIELD_BIT(a)) {
            struct vk_vertex_attribute_state attr = dyn->vi->attributes[a];
            for (uint32_t v = 0; v < l->n_vb_layouts; ++v) {
               if (attr.binding != l->vb_layouts[v].binding)
                  continue;
               attributes[p.attribute_count++] = (struct kk_dgc_attribute){
                   l->vb_layouts[v].src_offset_B,
                   slot,
                   attr.binding,
                   attr.offset,
                   util_format_get_blocksize(
                       vk_format_to_pipe_format(attr.format)),
                   1,
               };
            }
         }
         ++slot;
      }
   }
   if (p.attribute_count)
      p.attributes =
          kk_pool_upload(cmd, attributes,
                         p.attribute_count * sizeof(attributes[0]), 8)
              .gpu;
   return p;
}

VKAPI_ATTR void VKAPI_CALL kk_CmdPreprocessGeneratedCommandsEXT(
    VkCommandBuffer commandBuffer, const VkGeneratedCommandsInfoEXT *info,
    VkCommandBuffer stateCommandBuffer)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(kk_cmd_buffer, state, stateCommandBuffer);
   if (!info->maxSequenceCount)
      return;
   if (!kk_dgc_init(kk_cmd_buffer_device(cmd))) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INITIALIZATION_FAILED);
      return;
   }
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout,
                  info->indirectCommandsLayout);
   /* Execution must bind the same descriptors, push values and graphics state.
    * Keep only GPU token data in the persistent preprocess buffer. Internal
    * descriptor uploads then belong to the executing command buffer. */
   struct kk_dgc_params p = {
      .stream = info->indirectAddress,
      .count = info->sequenceCountAddress,
      .output = info->preprocessAddress,
      .maximum = info->maxSequenceCount,
      .stream_stride = layout->vk.stride,
      .record_stride = layout->token_span,
      .args_offset = SNAPSHOT_HEADER,
      .execution_range = info->preprocessAddress + SNAPSHOT_COUNT,
   };
   uint64_t parameters = kk_pool_upload(cmd, &p, sizeof(p), 8).gpu;
   if (!parameters)
      return;
   if (!kk_dgc_encode(cmd, cs_get_compute(cmd), parameters, p.maximum, 0))
      return;
   mtl_compute_set_argument_table(cmd->metal.compute, cmd->argument_table);
   kk_cmd_bind_root_to_argument_table(cmd, cmd->state.root_addr);
}

void kk_dgc_set_linked_table_buffers(struct kk_cmd_buffer *cmd,
   uint64_t vertex_buffer, uint64_t fragment_buffer)
{
   cmd->state.gfx.dgc_linked_vs_buffer = vertex_buffer;
   cmd->state.gfx.dgc_linked_fs_buffer = fragment_buffer;
}

static void kk_dgc_set_graphics_indirect(struct kk_cmd_buffer *cmd, bool indirect)
{
   if (cmd->state.gfx.dgc_icb == indirect) return;
   cmd->state.gfx.dgc_icb = indirect;
   cmd->state.dirty_shaders |= BITFIELD_BIT(MESA_SHADER_VERTEX);
}

static void kk_dgc_execute_parameters(struct kk_cmd_buffer *cmd,
                                      struct kk_dgc_params p, bool compute)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if (!kk_dgc_init(dev)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INITIALIZATION_FAILED);
      return;
   }
   kk_sparse_refresh_shader(cmd, compute ? MESA_SHADER_COMPUTE : MESA_SHADER_VERTEX);
   if (!p.base_root || !p.base_draw || (p.patch_count && !p.patches) ||
       (p.attribute_count && !p.attributes))
      return;
   /* Vulkan index tokens may select UINT8 on the GPU. Metal ICBs accept
    * UINT16/UINT32, so resolve Vulkan tokens through the shared Poly index
    * normalization. DXGI tokens can retain native ICB execution. */
   mtl_compute_pipeline_state *compute_pipeline = compute
      ? kk_dgc_get_compute_pipeline(dev, cmd->state.shaders[MESA_SHADER_COMPUTE]) : NULL;
   if (compute && !compute_pipeline) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INVALID_SHADER_NV);
      return;
   }
   /* Aliased attachments require the ordinary producer/cached-raster
    * boundary. A native user-stage ICB cannot be replayed per layer. */
   const bool layered = !compute && cmd->state.gfx.render.layered_bc;
   bool poly = !compute &&
               (layered || kk_dgc_needs_poly(cmd) ||
                (p.action == 1 &&
                 (p.index_size == 1 ||
                  (p.index_token != UINT32_MAX && !p.dx_index))));
   if (layered)
      kk_dgc_set_graphics_indirect(cmd, false);
   uint32_t maximum = p.maximum;
   p.execution_range = kk_pool_alloc(cmd, 8, 4).gpu;
   if (!p.execution_range)
      return;
   void *icb =
       poly ? NULL
            : kk_dgc_new_icb(dev, MIN2(maximum, KK_DGC_ICB_SLICE), compute);
   if (!poly && !icb) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }
   if (icb) {
      kk_device_add_buffer_to_residency_set(dev, icb);
      util_dynarray_append(&cmd->dgc_commands, icb);
   }
   uint64_t scratch =
       kk_pool_alloc(
           cmd, (uint64_t)MIN2(maximum, KK_DGC_ICB_SLICE) * p.record_stride, 64)
           .gpu;
   if (!scratch)
      return;
   if (!compute && !poly) {
      kk_dgc_set_graphics_indirect(cmd, true);
      kk_dgc_flush_graphics(cmd);
      p.linked_vs_buffer = cmd->state.gfx.dgc_linked_vs_buffer;
      p.linked_fs_buffer = cmd->state.gfx.dgc_linked_fs_buffer;
      p.interlock_guard = dev->interlock_guard ? dev->interlock_guard->gpu : 0;
   }
   for (uint32_t first = 0; first < maximum; first += KK_DGC_ICB_SLICE) {
      uint32_t count = MIN2(maximum - first, KK_DGC_ICB_SLICE);
      p.output = scratch;
      p.first = first;
      struct {
         struct kk_dgc_params p;
         uint64_t id;
      } data = {p, icb ? kk_dgc_icb_id(icb) : 0};
      data.p.maximum = count;
      uint64_t parameters = kk_pool_upload(cmd, &data, sizeof(data), 8).gpu;
      if (!parameters)
         return;
      if (!kk_dgc_encode(cmd, cs_get_compute(cmd), parameters, count, poly ? 1 : 2))
         break;
      if (poly) {
         mtl_compute_set_argument_table(cmd->metal.compute,
                                        cmd->argument_table);
         for (uint32_t i = 0; i < count; ++i) {
            uint64_t record = scratch + (uint64_t)i * p.record_stride;
            if (layered)
               kk_dgc_execute_layered_sequence(cmd, record,
                  record + p.args_offset, record + p.index_offset,
                  p.action == 1, p.ordinary ? first + i : 0);
            else
               kk_dgc_draw_sequence(cmd, record, record + p.args_offset,
                  record + p.index_offset, p.action == 1,
                  p.ordinary ? first + i : 0);
            if (cmd->vk.record_result != VK_SUCCESS)
               break;
         }
         if (cmd->vk.record_result != VK_SUCCESS)
            break;
         continue;
      }
      if (compute) {
         kk_dgc_execute_compute(
             cs_get_compute(cmd), icb, p.execution_range,
             compute_pipeline);
         mtl_barrier_after_encoder_stages(
             cmd->metal.compute, MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
      } else {
         kk_dgc_set_graphics_indirect(cmd, true);
         kk_dgc_flush_graphics(cmd);
         kk_dgc_execute_graphics(cs_get_render(cmd), icb, p.execution_range);
      }
   }
   if (!compute) kk_dgc_set_graphics_indirect(cmd, false);
   if (cmd->metal.compute)
      mtl_compute_set_argument_table(cmd->metal.compute, cmd->argument_table);
   kk_cmd_bind_root_to_argument_table(cmd, cmd->state.root_addr);
}
VKAPI_ATTR void VKAPI_CALL kk_CmdExecuteGeneratedCommandsEXT(
    VkCommandBuffer commandBuffer, VkBool32 preprocessed,
    const VkGeneratedCommandsInfoEXT *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   if (!info->maxSequenceCount)
      return;
   bool compute = info->shaderStages & VK_SHADER_STAGE_COMPUTE_BIT;
   if (!compute)
      kk_dgc_flush_graphics(cmd);
   if (!preprocessed)
      kk_CmdPreprocessGeneratedCommandsEXT(commandBuffer, info, commandBuffer);
   if (cmd->vk.record_result != VK_SUCCESS)
      return;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   VK_FROM_HANDLE(kk_indirect_commands_layout, layout,
                  info->indirectCommandsLayout);
   struct kk_dgc_params p = kk_dgc_state(cmd, cmd, info);
   p.stream = info->preprocessAddress + SNAPSHOT_HEADER;
   p.count = info->preprocessAddress + SNAPSHOT_COUNT;
   p.stream_stride = layout->token_span;
   struct kk_descriptor_state *desc =
       compute ? &cmd->state.cs.descriptors : &cmd->state.gfx.descriptors;
   p.root_size =
       offsetof(struct kk_root_descriptor_table, push) + desc->push_size;
   for (uint32_t i = 0; i < layout->vk.n_pc_layouts; ++i)
      p.root_size =
          MAX2(p.root_size, offsetof(struct kk_root_descriptor_table, push) +
                                layout->vk.pc_layouts[i].dst_offset_B +
                                layout->vk.pc_layouts[i].size_B);
   if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_SI))
      p.root_size =
          MAX2(p.root_size, offsetof(struct kk_root_descriptor_table, push) +
                                layout->vk.si_layout.dst_offset_B + 4);
   p.draw_offset = ALIGN_POT(p.root_size, 8);
   p.args_offset =
       ALIGN_POT(p.draw_offset + sizeof(struct kk_per_draw_data), 8);
   p.index_offset = ALIGN_POT(p.args_offset + 20, 8);
   p.vertex_input_offset = ALIGN_POT(p.index_offset + 16, 8);
   p.record_stride = ALIGN_POT(p.vertex_input_offset + p.vertex_input_size, 64);
   kk_dgc_execute_parameters(cmd, p, compute);
}

void kk_dgc_release_commands(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   util_dynarray_foreach(&cmd->dgc_commands, void *, icb)
   {
      kk_device_remove_buffer_from_residency_set(dev, *icb);
      mtl_release(*icb);
   }
   util_dynarray_clear(&cmd->dgc_commands);
}

bool kk_dgc_draw_indirect_count(struct kk_cmd_buffer *cmd,
                                const VkDrawIndirectCount2InfoKHR *info,
                                bool indexed)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   bool ready = kk_dgc_init(dev);
   if (!ready) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INITIALIZATION_FAILED);
      return true;
   }
   kk_dgc_flush_graphics(cmd);
   struct kk_indirect_commands_layout layout = {0};
   layout.vk.stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
   layout.vk.stride = info->addressRange.stride;
   layout.vk.dgc_info =
       BITFIELD_BIT(indexed ? MESA_VK_DGC_DRAW_INDEXED : MESA_VK_DGC_DRAW);
   VkGeneratedCommandsInfoEXT generated = {
       .indirectCommandsLayout = kk_indirect_commands_layout_to_handle(&layout),
       .indirectAddress = info->addressRange.address,
       .sequenceCountAddress = info->countAddressRange.address,
       .maxSequenceCount = info->maxDrawCount,
   };
   struct kk_dgc_params p = kk_dgc_state(cmd, cmd, &generated);
   p.ordinary = 1;
   p.root_size = offsetof(struct kk_root_descriptor_table, push) +
                 cmd->state.gfx.descriptors.push_size;
   p.draw_offset = ALIGN_POT(p.root_size, 8);
   p.args_offset =
       ALIGN_POT(p.draw_offset + sizeof(struct kk_per_draw_data), 8);
   p.index_offset = ALIGN_POT(p.args_offset + 20, 8);
   p.vertex_input_offset = ALIGN_POT(p.index_offset + 16, 8);
   p.record_stride = ALIGN_POT(p.vertex_input_offset + p.vertex_input_size, 64);
   kk_dgc_execute_parameters(cmd, p, false);
   return true;
}

uint64_t kk_dgc_offset_count(struct kk_cmd_buffer *cmd, uint64_t address,
                             uint32_t first)
{
   if (!first)
      return address;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   bool ready = kk_dgc_init(dev);
   if (!ready) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INITIALIZATION_FAILED);
      return 0;
   }
   struct kk_dgc_params p = {.count = address, .first = first};
   p.output = kk_pool_alloc(cmd, 4, 4).gpu;
   uint64_t parameters = kk_pool_upload(cmd, &p, sizeof(p), 8).gpu;
   if (!p.output || !parameters)
      return 0;
   if (!kk_dgc_encode(cmd, cs_get_compute(cmd), parameters, 1, 3))
      return 0;
   mtl_compute_set_argument_table(cmd->metal.compute, cmd->argument_table);
   kk_cmd_bind_root_to_argument_table(cmd, cmd->state.root_addr);
   return p.output;
}

/* The extension's entry points remain discoverable even when the optional
 * binding-stage masks are empty. No execution-set update has valid usage in
 * that profile: creation cannot produce an execution-set handle. */
VKAPI_ATTR void VKAPI_CALL kk_UpdateIndirectExecutionSetPipelineEXT(
    VkDevice device, VkIndirectExecutionSetEXT set, uint32_t count,
    const VkWriteIndirectExecutionSetPipelineEXT *writes)
{
   UNREACHABLE("Indirect pipeline binding is not supported");
}

VKAPI_ATTR void VKAPI_CALL kk_UpdateIndirectExecutionSetShaderEXT(
    VkDevice device, VkIndirectExecutionSetEXT set, uint32_t count,
    const VkWriteIndirectExecutionSetShaderEXT *writes)
{
   UNREACHABLE("Indirect shader binding is not supported");
}
