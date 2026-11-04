/* SPDX-License-Identifier: MIT */
#include "kk_depth_stencil_state.h"
#include "kk_image.h"
#include "kk_image_view.h"
#include "kk_physical_device.h"
#include "kk_shader.h"
#include "kosmickrisp/bridge/mtl_bridge.h"

struct kk_depth_stencil_record {
   struct kk_depth_stencil_record *next;
   struct kk_depth_stencil_snapshot snapshot;
   mtl_depth_stencil_state *read_only, *bypass;
};

static const struct kk_image *
kk_depth_stencil_image(const struct kk_attachment *attachment)
{
   return attachment->iview
      ? container_of(attachment->iview->vk.image, struct kk_image, vk) : NULL;
}

static bool
kk_depth_stencil_memoryless(const struct kk_attachment *attachment)
{
   const struct kk_image *image = kk_depth_stencil_image(attachment);
   if (!image)
      return false;
   const struct kk_image_plane *plane =
      &image->planes[attachment->iview->planes[0].image_plane];
   return plane->memoryless && !kk_image_plane_is_committed(plane);
}

VkResult
kk_cmd_capture_depth_stencil(struct kk_cmd_buffer *cmd,
                            struct kk_shader *actual_producer,
                            const struct kk_depth_stencil_snapshot **out)
{
   struct kk_graphics_state *gfx = &cmd->state.gfx;
   assert(actual_producer && actual_producer == gfx->shader);
   assert(cmd->metal.render);
   *out = NULL;
   struct kk_depth_stencil_record *record = vk_zalloc(
      &cmd->vk.pool->alloc, sizeof(*record), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!record)
      return vk_error(kk_cmd_buffer_device(cmd), VK_ERROR_OUT_OF_HOST_MEMORY);

   struct kk_depth_stencil_snapshot *s = &record->snapshot;
   *s = (struct kk_depth_stencil_snapshot){
      .command = cmd, .producer = actual_producer,
      .encoder = mtl_retain(cmd->metal.render),
      .selected = mtl_retain(kk_cmd_selected_depth_stencil(cmd, actual_producer)),
      .ds = cmd->vk.dynamic_graphics_state.ds,
      .render = gfx->render,
      .rasterization_samples = cmd->vk.dynamic_graphics_state.ms.rasterization_samples,
      .dynamic = gfx->is_depth_stencil_dynamic,
      .native_depth_bounds = kk_device_physical(kk_cmd_buffer_device(cmd))->vk.supported_features.depthBounds,
   };
   const struct kk_attachment *depth = &s->render.depth_att;
   const struct kk_attachment *stencil = &s->render.stencil_att;
   s->depth_image = kk_depth_stencil_image(depth);
   s->stencil_image = kk_depth_stencil_image(stencil);
   if (depth->iview) {
      s->present_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (actual_producer->info.vs.d_format != MTL_PIXEL_FORMAT_INVALID)
         s->tested_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (vk_image_layout_is_read_only(depth->image_layout, VK_IMAGE_ASPECT_DEPTH_BIT))
         s->read_only_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (kk_depth_stencil_memoryless(depth))
         s->memoryless_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
      s->depth_texture = mtl_retain(kk_transient_render_texture(depth->iview));
   }
   if (stencil->iview) {
      s->present_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (actual_producer->info.vs.s_format != MTL_PIXEL_FORMAT_INVALID)
         s->tested_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (vk_image_layout_is_read_only(stencil->image_layout, VK_IMAGE_ASPECT_STENCIL_BIT))
         s->read_only_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (kk_depth_stencil_memoryless(stencil))
         s->memoryless_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
      s->stencil_texture = mtl_retain(kk_transient_render_texture(stencil->iview));
   }
   for (unsigned i = 0; i < s->render.color_att_count; ++i)
      if (kk_depth_stencil_memoryless(&s->render.color_att[i]))
         s->memoryless_color_mask |= BITFIELD_BIT(i);

   record->next = cmd->depth_stencil_records;
   /* BYPASS has no enabled tests or writes, independent of the snapshot's
    * attachments and references. Move its command-owned cache to the head. */
   if (record->next) {
      record->bypass = record->next->bypass;
      record->next->bypass = NULL;
   }
   cmd->depth_stencil_records = record;
   *out = s;
   return VK_SUCCESS;
}

mtl_depth_stencil_state *
kk_cmd_depth_stencil_variant(const struct kk_depth_stencil_snapshot *s,
                            enum kk_depth_stencil_mode mode)
{
   assert(mode <= KK_DEPTH_STENCIL_BYPASS);
   if (mode == KK_DEPTH_STENCIL_ORIGINAL)
      return s->selected;
   struct kk_depth_stencil_record *record =
      container_of(s, struct kk_depth_stencil_record, snapshot);
   mtl_depth_stencil_state **slot = mode == KK_DEPTH_STENCIL_READ_ONLY
      ? &record->read_only : &s->command->depth_stencil_records->bypass;
   if (!*slot) {
      struct vk_depth_stencil_state ds = s->ds;
      ds.depth.write_enable = false;
      ds.stencil.write_enable = false;
      ds.stencil.front.write_mask = ds.stencil.back.write_mask = 0;
      ds.stencil.front.op.fail = ds.stencil.front.op.pass =
         ds.stencil.front.op.depth_fail = VK_STENCIL_OP_KEEP;
      ds.stencil.back.op.fail = ds.stencil.back.op.pass =
         ds.stencil.back.op.depth_fail = VK_STENCIL_OP_KEEP;
      if (mode != KK_DEPTH_STENCIL_READ_ONLY) {
         ds.depth.test_enable = false;
         ds.stencil.test_enable = false;
      }
      *slot = kk_compile_depth_stencil_state(kk_cmd_buffer_device(s->command),
         &ds, s->tested_aspects & VK_IMAGE_ASPECT_DEPTH_BIT,
         s->tested_aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
   }
   return *slot;
}

VkResult
kk_cmd_bind_depth_stencil(const struct kk_depth_stencil_snapshot *s,
                          enum kk_depth_stencil_mode mode)
{
   /* Retaining an ended encoder cannot preserve its memoryless contents. */
   assert(s->command->metal.render == s->encoder);
   mtl_depth_stencil_state *state = kk_cmd_depth_stencil_variant(s, mode);
   if (!state) {
      vk_command_buffer_set_error(&s->command->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   mtl_set_depth_stencil_state(s->encoder, state);
   mtl_set_stencil_references(s->encoder, s->ds.stencil.front.reference,
                             s->ds.stencil.back.reference);
   if (s->native_depth_bounds) {
      bool bounds = mode != KK_DEPTH_STENCIL_BYPASS && s->ds.depth.bounds_test.enable;
      mtl_set_depth_test_bounds(s->encoder,
         bounds ? s->ds.depth.bounds_test.min : 0.0f,
         bounds ? s->ds.depth.bounds_test.max : 1.0f);
   }
   return VK_SUCCESS;
}

void
kk_cmd_depth_stencil_finish(struct kk_cmd_buffer *cmd)
{
   while (cmd->depth_stencil_records) {
      struct kk_depth_stencil_record *record = cmd->depth_stencil_records;
      cmd->depth_stencil_records = record->next;
      mtl_release(record->snapshot.selected);
      mtl_release(record->snapshot.encoder);
      mtl_release(record->snapshot.depth_texture);
      mtl_release(record->snapshot.stencil_texture);
      mtl_release(record->read_only);
      mtl_release(record->bypass);
      vk_free(&cmd->vk.pool->alloc, record);
   }
}
