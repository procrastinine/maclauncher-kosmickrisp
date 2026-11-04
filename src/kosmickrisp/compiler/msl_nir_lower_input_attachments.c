/*
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "msl_private.h"
#include "nir_builder.h"
#include "nir_to_msl.h"

#include "vk_graphics_state.h"

struct lower_ia_state {
   const struct vk_input_attachment_location_state *ial;
   const struct vk_color_attachment_location_state *cal;
   const struct vk_render_pass_state *rp;
   bool interlock_shadow_samples;
};

#include "msl_attachment_samples.h"

static bool
lower_ia(nir_builder *b, nir_intrinsic_instr *intrin,
         const struct lower_ia_state *state)
{
   return msl_attachment_sample_fetch(b, intrin, state);
}

static bool
lower_input_attachment(struct nir_builder *b, nir_intrinsic_instr *intrin,
                       void *data)
{
   if (intrin->intrinsic != nir_intrinsic_image_deref_input_attachment_load)
      return false;

   return lower_ia(b, intrin, (const struct lower_ia_state *)data);
}

static bool
msl_nir_lower_frag_input_attachments(nir_shader *nir,
                                     const struct lower_ia_state *state)
{
   return nir_shader_intrinsics_pass(nir, lower_input_attachment,
                                     nir_metadata_control_flow, (void *)state);
}

void
msl_nir_lower_input_attachments(
   nir_shader *nir, const struct vk_input_attachment_location_state *ial,
   const struct vk_color_attachment_location_state *cal,
   const struct vk_render_pass_state *rp, bool interlock_shadow_samples)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   if (ial) {
      struct lower_ia_state state = {
         .ial = ial,
         .cal = cal,
         .rp = rp,
         .interlock_shadow_samples = interlock_shadow_samples,
      };
      NIR_PASS(_, nir, msl_nir_lower_frag_input_attachments, &state);
   }

   /* Lower the remaining depth/stencil, unmapped and other-sample
    * reads through texture descriptors. */
   nir_input_attachment_options input_attachment_options = {};
   NIR_PASS(_, nir, nir_lower_input_attachments, &input_attachment_options);
}
