/* SPDX-License-Identifier: MIT
 * Stored-depth snapshot technique: metalsharp/VKD3D-Proton-MacOS
 * 4e89065e359f9c6c2a78170b045835d6382113d9, metalsharp-vkmt.diff.
 * Tests and ordered updates use the shared KosmicKrisp DS transaction. */
/* Stored-depth technique reference: metalsharp/VKD3D-Proton-MacOS,
 * 4e89065e359f9c6c2a78170b045835d6382113d9, moltenvk/metalsharp-vkmt.diff
 * and spirv-cross/metalsharp.diff. This uses the existing KosmicKrisp
 * transaction; no reference patch source is incorporated. */
#include "kk_depth_bounds.h"
#include "kk_interlock_envelope.h"
#include "kk_cmd_buffer.h"

void
kk_depth_bounds_set_transaction_state(struct kk_interlock_envelope *env,
   const struct vk_depth_stencil_state *ds, VkImageAspectFlags aspects)
{
   const bool depth = aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
   env->depth_enable = depth && ds->depth.test_enable;
   env->depth_write = depth && ds->depth.write_enable;
   env->depth_compare = ds->depth.compare_op;
   env->bounds_enable = depth && ds->depth.bounds_test.enable;
   env->bounds_min = ds->depth.bounds_test.min;
   env->bounds_max = ds->depth.bounds_test.max;
   env->stencil_enable = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
      ds->stencil.test_enable;
   const struct vk_stencil_test_face_state *faces[] = {
      &ds->stencil.front, &ds->stencil.back};
   for (unsigned i = 0; i < 2; ++i) {
      const struct vk_stencil_test_face_state *s = faces[i];
      env->stencil_state[i] = (struct kk_interlock_stencil_state){
         s->reference, s->compare_mask, s->write_mask, s->op.compare,
         s->op.fail, s->op.depth_fail, s->op.pass};
   }
}

VkImageAspectFlags
kk_depth_bounds_write_aspects(const struct vk_depth_stencil_state *ds,
   VkImageAspectFlags tested, VkImageAspectFlags read_only)
{
   VkImageAspectFlags result = 0;
   if (ds->depth.test_enable && ds->depth.write_enable)
      result |= VK_IMAGE_ASPECT_DEPTH_BIT;
   const struct vk_stencil_test_face_state *faces[] = {&ds->stencil.front, &ds->stencil.back};
   for (unsigned i = 0; i < 2; ++i) {
      const struct vk_stencil_test_face_state *face = faces[i];
      if (ds->stencil.test_enable && face->write_mask &&
          (face->op.fail != VK_STENCIL_OP_KEEP || face->op.pass != VK_STENCIL_OP_KEEP ||
           face->op.depth_fail != VK_STENCIL_OP_KEEP))
         result |= VK_IMAGE_ASPECT_STENCIL_BIT;
   }
   return result & tested & ~read_only;
}

void
kk_depth_bounds_set_transaction_inputs(struct kk_interlock_envelope *env,
   const struct vk_input_attachment_location_state *inputs,
   const struct kk_rendering_state *render)
{
   struct kk_depth_bounds_envelope *mapped =
      container_of(env, struct kk_depth_bounds_envelope, base);
   mapped->input_depth_index = render->depth_att.iview ? inputs->depth_att : MESA_VK_ATTACHMENT_UNUSED;
   mapped->input_stencil_index = render->stencil_att.iview ? inputs->stencil_att : MESA_VK_ATTACHMENT_UNUSED;
   for (unsigned i = 0; i < 8; ++i) mapped->input_color_indices[i] = MESA_VK_ATTACHMENT_UNUSED;
   for (unsigned i = 0; i < MIN2(render->color_att_count, inputs->color_attachment_count); ++i) {
      unsigned logical = render->color_map[i];
      if (logical != MESA_VK_ATTACHMENT_UNUSED && render->color_att[i].iview)
         mapped->input_color_indices[logical] = inputs->color_map[i];
   }
}
