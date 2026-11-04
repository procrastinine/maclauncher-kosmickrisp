// SPDX-License-Identifier: MIT
/* Dynamic rendering continuations share their encoder until a real split. */
#ifndef KK_WHOLE_PASS_H
#define KK_WHOLE_PASS_H
#include "vk_pipeline.h"
#include "kk_image_view.h"
#include "kk_msrtss_state.h"

VkAccessFlags2
kk_whole_pass_attachment_access(VkAccessFlags2 access,
                                VkPipelineStageFlags2 stages)
{
   const VkPipelineStageFlags2 color =
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
   const VkPipelineStageFlags2 depth_stencil =
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
   if (stages & ~(color | depth_stencil))
      return access;
   VkAccessFlags2 result = access &
      ~(VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
   if (access & VK_ACCESS_2_MEMORY_READ_BIT) {
      if (stages & color)
         result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
      if (stages & depth_stencil)
         result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
   }
   if (access & VK_ACCESS_2_MEMORY_WRITE_BIT) {
      if (stages & color)
         result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      if (stages & depth_stencil)
         result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   }
   return result;
}

static VkSampleCountFlagBits
kk_whole_pass_implicit_samples(const VkRenderingInfo *info)
{
   return kk_msrtss_samples(info);
}

static void
kk_whole_pass_save(struct kk_cmd_buffer *cmd, const VkRenderingInfo *info)
{
   struct kk_whole_pass_state *pass = &cmd->whole_pass;
   pass->suspended = false;
   pass->area = info->renderArea;
   pass->layers = info->layerCount;
   pass->view_mask = info->viewMask;
   pass->color_count = info->colorAttachmentCount;
   for (uint32_t i = 0; i < pass->color_count; i++)
      pass->colors[i] = info->pColorAttachments[i].imageView;
   pass->depth = info->pDepthAttachment
                    ? info->pDepthAttachment->imageView : VK_NULL_HANDLE;
   pass->stencil = info->pStencilAttachment
                      ? info->pStencilAttachment->imageView : VK_NULL_HANDLE;
   pass->implicit_samples = kk_whole_pass_implicit_samples(info);
}

static bool
kk_whole_pass_matches(const struct kk_whole_pass_state *pass,
                      const VkRenderingInfo *info)
{
   if (pass->color_count != info->colorAttachmentCount ||
       pass->layers != info->layerCount || pass->view_mask != info->viewMask ||
       memcmp(&pass->area, &info->renderArea, sizeof(pass->area)) ||
       pass->depth != (info->pDepthAttachment
                         ? info->pDepthAttachment->imageView : VK_NULL_HANDLE) ||
       pass->stencil != (info->pStencilAttachment
                           ? info->pStencilAttachment->imageView : VK_NULL_HANDLE) ||
       pass->implicit_samples != kk_whole_pass_implicit_samples(info))
      return false;
   for (uint32_t i = 0; i < pass->color_count; i++)
      if (pass->colors[i] != info->pColorAttachments[i].imageView)
         return false;
   return true;
}

void
kk_whole_pass_flush_suspended(struct kk_cmd_buffer *cmd)
{
   if (!cmd->whole_pass.suspended)
      return;
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   cmd->whole_pass.suspended = false;
   const VkRenderingEndInfoKHR end = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_END_INFO_KHR,
   };
   /* Call the saved backend, including any implicit-attachment resolves. */
   dev->whole_pass_end_rendering(kk_cmd_buffer_to_handle(cmd), &end);
}

VKAPI_ATTR void VKAPI_CALL
kk_whole_pass_begin_rendering(VkCommandBuffer commandBuffer,
                              const VkRenderingInfo *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if ((info->flags & VK_RENDERING_RESUMING_BIT) &&
       cmd->whole_pass.suspended && cmd->metal.render &&
       kk_whole_pass_matches(&cmd->whole_pass, info)) {
      cmd->whole_pass.suspended = false;
      struct kk_rendering_state *render = &cmd->state.gfx.render;
      render->flags = info->flags;
      render->force_attachment_store =
         !render->rendering_to_whole_framebuffer ||
         (info->flags & VK_RENDERING_SUSPENDING_BIT);
      kk_init_rendering_locations(cmd, info);
      const bool uses_heap = cmd->uses_heap;
      kk_cmd_buffer_dirty_all_gfx(cmd);
      cmd->uses_heap = uses_heap;
      return;
   }

   kk_whole_pass_flush_suspended(cmd);
   dev->whole_pass_begin_rendering(commandBuffer, info);
   /* Only a suspended scope needs a matching resume snapshot. */
   if (info->flags & VK_RENDERING_SUSPENDING_BIT)
      kk_whole_pass_save(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL
kk_whole_pass_end_rendering2(VkCommandBuffer commandBuffer,
                             const VkRenderingEndInfoKHR *info)
{
   VK_FROM_HANDLE(kk_cmd_buffer, cmd, commandBuffer);
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   if ((cmd->state.gfx.render.flags & VK_RENDERING_SUSPENDING_BIT) &&
       cmd->metal.render) {
      cmd->whole_pass.suspended = true;
      return;
   }

   cmd->whole_pass.suspended = false;
   dev->whole_pass_end_rendering(commandBuffer, info);
}

VKAPI_ATTR void VKAPI_CALL
kk_whole_pass_end_rendering(VkCommandBuffer commandBuffer)
{
   const VkRenderingEndInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_END_INFO_KHR,
   };
   kk_whole_pass_end_rendering2(commandBuffer, &info);
}

enum kk_whole_pass_barrier_scope
kk_whole_pass_classify_barrier(const VkDependencyInfo *dep)
{
   const VkPipelineStageFlags2 stages =
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
   const VkAccessFlags2 access =
      VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   if (!(dep->dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT) ||
       dep->bufferMemoryBarrierCount)
      return KK_WHOLE_PASS_BARRIER_NONE;
   bool attachment_only = true;
   for (uint32_t i = 0; i < dep->memoryBarrierCount; i++) {
      const VkMemoryBarrier2 *b = &dep->pMemoryBarriers[i];
      attachment_only &= !((b->srcStageMask | b->dstStageMask) &
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
      if (((b->srcStageMask | b->dstStageMask) & ~stages) ||
          ((kk_whole_pass_attachment_access(b->srcAccessMask, b->srcStageMask) |
            kk_whole_pass_attachment_access(b->dstAccessMask, b->dstStageMask)) & ~access))
         return KK_WHOLE_PASS_BARRIER_NONE;
   }
   for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; i++) {
      const VkImageMemoryBarrier2 *b = &dep->pImageMemoryBarriers[i];
      attachment_only &= !((b->srcStageMask | b->dstStageMask) &
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
      if (((b->srcStageMask | b->dstStageMask) & ~stages) ||
          ((kk_whole_pass_attachment_access(b->srcAccessMask, b->srcStageMask) |
            kk_whole_pass_attachment_access(b->dstAccessMask, b->dstStageMask)) & ~access))
         return KK_WHOLE_PASS_BARRIER_NONE;
   }
   return KK_WHOLE_PASS_BARRIER_LOCAL |
          (attachment_only ? KK_WHOLE_PASS_BARRIER_ATTACHMENT : 0);
}

void
kk_whole_pass_command_boundary(struct kk_cmd_buffer *cmd)
{
   const bool keep_render = cmd->whole_pass.suspended && cmd->metal.render;
   const struct kk_rendering_state render = cmd->state.gfx.render;
   mtl_render_pass_descriptor *descriptor =
      cmd->state.gfx.render_pass_descriptor;
   const enum vk_rp_attachment_flags attachments =
      cmd->vk.dynamic_graphics_state.rp.attachments;
   const uint32_t view_mask = cmd->vk.dynamic_graphics_state.rp.view_mask;
   const bool uses_heap = cmd->uses_heap;
   if (!keep_render) {
      cs_end(cmd);
      kk_render_pass_release(cmd, descriptor);
   }
   kk_cmd_release_dynamic_ds_state(cmd);
   kk_descriptor_state_fini(cmd, &cmd->state.gfx.descriptors);
   kk_descriptor_state_fini(cmd, &cmd->state.cs.descriptors);
   memset(&cmd->state, 0, sizeof(cmd->state));
   vk_dynamic_graphics_state_clear(&cmd->vk.dynamic_graphics_state);
   if (keep_render) {
      cmd->state.gfx.render = render;
      cmd->state.gfx.render_pass_descriptor = descriptor;
      cmd->vk.dynamic_graphics_state.rp.attachments = attachments;
      cmd->vk.dynamic_graphics_state.rp.view_mask = view_mask;
      cmd->vk.dynamic_graphics_state.cb.attachment_count = render.color_att_count;
   }
   kk_cmd_buffer_dirty_all_gfx(cmd);
   if (keep_render)
      cmd->uses_heap = uses_heap;
}

/* Metal orders fixed-function color and depth/stencil attachment accesses
 * per framebuffer sample across draws. A resource-alias barrier is unrelated
 * to this dependency and can force tile contents out of memoryless storage. */
bool
kk_whole_pass_attachment_barrier(const VkDependencyInfo *dep)
{
   return kk_whole_pass_classify_barrier(dep) & KK_WHOLE_PASS_BARRIER_ATTACHMENT;
}

/* Inspect immutable pipeline state without binding shaders, descriptors or
 * a scratch command buffer. Only fixed native rasterization proves safety. */
static bool
kk_whole_pass_pipeline_native(VkPipeline handle)
{
   VK_FROM_HANDLE(vk_pipeline, pipeline, handle);
   if (pipeline->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS ||
       pipeline->stages & ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT))
      return false;
   const struct vk_dynamic_graphics_state *dyn =
      vk_pipeline_get_dynamic_graphics_state(pipeline);
   if (!dyn)
      return false;
   const struct vk_shader *vk_vs = vk_pipeline_get_shader(pipeline, MESA_SHADER_VERTEX);
   const struct vk_shader *vk_fs = vk_pipeline_get_shader(pipeline, MESA_SHADER_FRAGMENT);
   const struct kk_shader *vs = vk_vs ? container_of(vk_vs, struct kk_shader, vk) : NULL;
   const struct kk_shader *fs = vk_fs ? container_of(vk_fs, struct kk_shader, vk) : NULL;
   if (!vs || vs->info.has_geometry || vs->info.polygon_point ||
       vs->info.conservative || vs->info.poly_layer_replay ||
       vs->info.interlock_envelope || vs->info.depth_bounds_emulation ||
       vs->interlock || vs->interlock_pipeline || vs->info.vs.fsr_coarse ||
       (fs && (fs->info.fs.emulates_per_vertex_inputs ||
               fs->info.interlock_envelope || fs->info.depth_bounds_emulation ||
               fs->interlock || fs->interlock_pipeline)) ||
       !BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) ||
       !BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_RS_POLYGON_MODE) ||
       !BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_RS_DEPTH_BIAS_ENABLE) ||
       !BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_MS_SAMPLE_LOCATIONS_ENABLE) ||
       dyn->rs.polygon_mode != VK_POLYGON_MODE_FILL ||
       dyn->rs.depth_bias.enable || dyn->ms.sample_locations_enable ||
       (fs && fs->info.fs.uses_flat_varyings &&
        BITSET_TEST(dyn->set, MESA_VK_DYNAMIC_RS_PROVOKING_VERTEX) &&
        dyn->rs.provoking_vertex != VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT))
      return false;
   /* MS Bresenham lines change the pass's sample locations at the first draw. */
   return dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
          dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
          dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
}

/* Writable MSAA conversions may break the encoder for the whole mixed pass. */
static bool
kk_whole_pass_writable_msaa(VkImageView handle)
{
   if (handle == VK_NULL_HANDLE)
      return false;
   VK_FROM_HANDLE(kk_image_view, view, handle);
   return view->vk.image->samples > VK_SAMPLE_COUNT_1_BIT &&
          (view->vk.image->usage & VK_IMAGE_USAGE_STORAGE_BIT);
}

bool
kk_whole_pass_transient_safe(struct vk_command_buffer **commands, uint32_t count)
{
   VkPipeline last_pipeline = VK_NULL_HANDLE;
   bool last_pipeline_native = false;
   for (uint32_t c = 0; c < count; c++) {
      bool inside = false;
      bool pipeline_native = false;
      list_for_each_entry(struct vk_cmd_queue_entry, entry,
                           &commands[c]->cmd_queue.cmds, cmd_link) {
         switch (entry->type) {
         case VK_CMD_BEGIN_RENDERING: {
            const VkRenderingInfo *info = entry->u.begin_rendering.rendering_info;
            for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
               if (kk_whole_pass_writable_msaa(info->pColorAttachments[i].imageView))
                  return false;
            if ((info->pDepthAttachment &&
                 kk_whole_pass_writable_msaa(info->pDepthAttachment->imageView)) ||
                (info->pStencilAttachment &&
                 kk_whole_pass_writable_msaa(info->pStencilAttachment->imageView)))
               return false;
            inside = true;
            break;
         }
         case VK_CMD_END_RENDERING:
         case VK_CMD_END_RENDERING2_KHR:
            inside = false;
            break;
         case VK_CMD_BIND_PIPELINE:
            if (last_pipeline != entry->u.bind_pipeline.pipeline) {
               last_pipeline = entry->u.bind_pipeline.pipeline;
               last_pipeline_native = kk_whole_pass_pipeline_native(last_pipeline);
            }
            pipeline_native = last_pipeline_native;
            if (!pipeline_native)
               return false;
            break;
         case VK_CMD_DRAW:
            if (!pipeline_native)
               return false;
            break;
         case VK_CMD_PIPELINE_BARRIER2:
            if (inside) {
               const VkDependencyInfo *dep = entry->u.pipeline_barrier2.dependency_info;
               struct kk_cmd_buffer plan = {0};
               plan.state.gfx.render.ds_write_available = true;
               if (!kk_whole_pass_attachment_barrier(dep) ||
                   kk_barrier_requires_encoder_split(&plan, dep))
                  return false;
            }
            break;
         case VK_CMD_SET_VIEWPORT:
         case VK_CMD_SET_VIEWPORT_WITH_COUNT:
         case VK_CMD_SET_SCISSOR:
         case VK_CMD_SET_SCISSOR_WITH_COUNT:
         case VK_CMD_PUSH_CONSTANTS:
         case VK_CMD_PUSH_CONSTANTS2:
         case VK_CMD_BIND_DESCRIPTOR_SETS:
         case VK_CMD_BIND_DESCRIPTOR_SETS2:
         case VK_CMD_BIND_VERTEX_BUFFERS:
         case VK_CMD_BIND_VERTEX_BUFFERS2:
         case VK_CMD_BEGIN_DEBUG_UTILS_LABEL_EXT:
         case VK_CMD_END_DEBUG_UTILS_LABEL_EXT:
         case VK_CMD_INSERT_DEBUG_UTILS_LABEL_EXT:
            break;
         case VK_CMD_COPY_IMAGE_TO_BUFFER:
         case VK_CMD_COPY_IMAGE_TO_BUFFER2:
         case VK_CMD_COPY_BUFFER:
         case VK_CMD_COPY_BUFFER2:
         case VK_CMD_FILL_BUFFER:
         case VK_CMD_UPDATE_BUFFER:
            if (inside)
               return false;
            break;
         default:
            /* Secondary, indirect, queries, dynamic pass changes and meta or
             * Poly work retain the ordinary private storage path. */
            return false;
         }
      }
   }
   return true;
}
#endif
