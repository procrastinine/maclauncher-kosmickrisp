/* SPDX-License-Identifier: MIT */
#ifndef KK_WRITABLE_MSAA_META_IMPL_H
#define KK_WRITABLE_MSAA_META_IMPL_H
#include "vk_format.h"
#include "vk_render_pass.h"

/* Included once in kk_cmd_meta.c to share its ordinary meta state preservation.
 * The sample arrays are the canonical storage representation. Native attachment
 * contents are loaded and published at physical encoder boundaries, never from
 * a global image dirty flag. Disjoint views and queues therefore share the same
 * immutable memory layout and synchronize through ordinary Vulkan barriers.
 */
static void
kk_writable_msaa_meta_copy(struct kk_cmd_buffer *cmd,
                           const VkCopyImageInfo2 *copy,
                           VkPipelineBindPoint bind)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   VK_FROM_HANDLE(kk_image, src, copy->srcImage);
   VK_FROM_HANDLE(kk_image, dst, copy->dstImage);
   const struct vk_meta_copy_image_properties src_properties = {
      .color.view_format = vk_format_from_pipe_format(
         kk_writable_msaa_raw_format(vk_format_to_pipe_format(src->vk.format))),
      .tile_size = {8, 8, 1},
   };
   const struct vk_meta_copy_image_properties dst_properties = {
      .color.view_format = vk_format_from_pipe_format(
         kk_writable_msaa_raw_format(vk_format_to_pipe_format(dst->vk.format))),
      .tile_size = {8, 8, 1},
   };
   cs_end(cmd);
   const struct kk_rendering_state render = cmd->state.gfx.render;
   mtl_render_pass_descriptor *descriptor = cmd->state.gfx.render_pass_descriptor;
   const bool need_start = cmd->state.gfx.need_to_start_render_pass;
   const uint32_t write_mask = cmd->writable_msaa_render_writes;
   const uint32_t attachment_mask = cmd->writable_msaa_render_attachments;
   const VkShaderStageFlags pipeline_stages = cmd->vk.pipeline_shader_stages;
   struct kk_meta_save save;
   cmd->writable_msaa_meta = true;
   kk_meta_begin(cmd, &save, bind, "meta:MSAA samples");
   vk_meta_copy_image(&cmd->vk, &dev->meta, copy, &src_properties, &dst_properties,
                      bind);
   /* No conversion encoder may survive into the application's render encoder.
    * This also inserts the existing Metal encoder fences in both directions.
    */
   cs_end(cmd);
   kk_meta_end(cmd, &save, bind);
   cmd->state.gfx.render = render;
   cmd->state.gfx.render_pass_descriptor = descriptor;
   cmd->state.gfx.need_to_start_render_pass = need_start;
   cmd->writable_msaa_render_writes = write_mask;
   cmd->writable_msaa_render_attachments = attachment_mask;
   cmd->vk.pipeline_shader_stages = pipeline_stages;
   cmd->writable_msaa_meta = false;
}

static void
kk_writable_msaa_convert(struct kk_cmd_buffer *cmd, struct kk_image *image,
                         uint32_t base_layer, uint32_t layer_count, bool publish)
{
   if (!image->writable_msaa || cmd->writable_msaa_meta || !layer_count)
      return;
   assert(base_layer <= image->vk.array_layers &&
          layer_count <= image->vk.array_layers - base_layer);
   const VkImage native = image->writable_msaa->native_image;
   const VkImage logical = kk_image_to_handle(image);
   const VkImageSubresourceLayers subres = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, base_layer, layer_count,
   };
   const VkImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = subres,
      .dstSubresource = subres,
      .extent = image->vk.extent,
   };
   const VkCopyImageInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
      .srcImage = publish ? native : logical,
      .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .dstImage = publish ? logical : native,
      .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .regionCount = 1,
      .pRegions = &region,
   };
   kk_writable_msaa_meta_copy(cmd, &copy,
      publish ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS);
}

bool
kk_writable_msaa_copy_image(struct kk_cmd_buffer *cmd,
                            const VkCopyImageInfo2 *copy)
{
   VK_FROM_HANDLE(kk_image, src, copy->srcImage);
   VK_FROM_HANDLE(kk_image, dst, copy->dstImage);
   if ((!src->writable_msaa && !dst->writable_msaa) || cmd->writable_msaa_meta)
      return false;
   kk_writable_msaa_meta_copy(cmd, copy,
      dst->writable_msaa ? VK_PIPELINE_BIND_POINT_COMPUTE
                         : VK_PIPELINE_BIND_POINT_GRAPHICS);
   return true;
}

static void
kk_writable_msaa_convert_view(struct kk_cmd_buffer *cmd,
                              const struct kk_image_view *view, bool publish)
{
   if (!view)
      return;
   struct kk_image *image = container_of(view->vk.image, struct kk_image, vk);
   if (!image->writable_msaa)
      return;
   struct kk_rendering_state *render = &cmd->state.gfx.render;
   const uint32_t base = view->vk.base_array_layer;
   if (render->view_mask) {
      u_foreach_bit(layer, render->view_mask)
         kk_writable_msaa_convert(cmd, image, base + layer, 1, publish);
   } else {
      kk_writable_msaa_convert(cmd, image, base,
         MIN2(render->layer_count, view->vk.layer_count), publish);
   }
}

void
kk_writable_msaa_prepare_render(struct kk_cmd_buffer *cmd)
{
   if (cmd->writable_msaa_meta)
      return;
   cmd->writable_msaa_render_writes = 0;
   cmd->writable_msaa_render_attachments = 0;
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      const struct kk_image_view *view = render->color_att[i].iview;
      if (!view || !container_of(view->vk.image, struct kk_image, vk)->writable_msaa)
         continue;
      cmd->writable_msaa_render_attachments |= BITFIELD_BIT(i);
      kk_writable_msaa_convert_view(cmd, render->color_att[i].iview, false);
      if (!render->writable_msaa_loaded &&
          render->color_att[i].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
         cmd->writable_msaa_render_writes |= BITFIELD_BIT(i);
   }
   if (cmd->writable_msaa_render_attachments)
      cmd->state.gfx.render.writable_msaa_loaded = true;

   /* Traditional subpasses may read an input that is not a color attachment.
    * Such a view still consumes the native primary representation, including
    * when its layout remains GENERAL across preceding storage writes.
    */
   if (cmd->vk.render_pass) {
      const struct vk_subpass *subpass =
         &cmd->vk.render_pass->subpasses[cmd->vk.subpass_idx];
      for (uint32_t i = 0; i < subpass->input_count; i++) {
         const uint32_t index = subpass->input_attachments[i].attachment;
         if (index == VK_ATTACHMENT_UNUSED)
            continue;
         const struct vk_image_view *input =
            vk_command_buffer_get_attachment_view(&cmd->vk, index);
         if (!input)
            continue;
         const struct kk_image_view *view = container_of(input,
                                                         struct kk_image_view, vk);
         bool prepared = false;
         for (uint32_t a = 0; a < render->color_att_count; a++)
            prepared |= view == render->color_att[a].iview;
         if (!prepared)
            kk_writable_msaa_convert_view(cmd, view, false);
      }
   }
}

void
kk_writable_msaa_publish_render(struct kk_cmd_buffer *cmd)
{
   if (cmd->writable_msaa_meta || !cmd->writable_msaa_render_attachments)
      return;
   const uint32_t writes = cmd->writable_msaa_render_writes;
   cmd->writable_msaa_render_writes = 0;
   u_foreach_bit(i, writes)
      kk_writable_msaa_convert_view(cmd,
         cmd->state.gfx.render.color_att[i].iview, true);
}

void
kk_writable_msaa_record_draw(struct kk_cmd_buffer *cmd)
{
   if (cmd->writable_msaa_meta || !cmd->writable_msaa_render_attachments)
      return;
   const struct kk_rendering_state *render = &cmd->state.gfx.render;
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const struct kk_shader *fs = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   if (!fs || dyn->rs.rasterizer_discard_enable)
      return;
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      const struct kk_image_view *view = render->color_att[i].iview;
      if (!view || !container_of(view->vk.image, struct kk_image, vk)->writable_msaa)
         continue;
      const uint8_t location = render->color_map[i];
      if (location != MESA_VK_ATTACHMENT_UNUSED &&
          (dyn->cb.color_write_enables & BITFIELD_BIT(i)) &&
          dyn->cb.attachments[i].write_mask &&
          (fs->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DATA0 + location)))
         cmd->writable_msaa_render_writes |= BITFIELD_BIT(i);
   }
}

#endif
