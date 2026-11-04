/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SELECT_H
#define KK_INTERLOCK_SELECT_H

#include "compiler/nir/nir_serialize.h"

static bool
kk_interlock_pixel_shader(const nir_shader *nir)
{
   return nir && nir->info.stage == MESA_SHADER_FRAGMENT &&
      (nir->info.fs.pixel_interlock_ordered ||
       nir->info.fs.pixel_interlock_unordered);
}

static bool
kk_interlock_multisample_pipeline(const struct vk_graphics_pipeline_state *state)
{
   return state && ((state->ms && state->ms->rasterization_samples > 1) ||
      BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_RASTERIZATION_SAMPLES));
}

/* Only the pixel envelope needs the producer and fragment in one compile.
 * Native single-sample pixel and sample interlock keep ordinary fast linking. */
static bool
kk_interlock_link_graphics(struct vk_device *device,
   const struct vk_graphics_pipeline_state *state,
   const struct blob *nir, const struct vk_shader *compiled)
{
   if (!device->enabled_features.fragmentShaderPixelInterlock)
      return false;
   if (state && state->ms && !kk_interlock_multisample_pipeline(state))
      return false;
   if (nir) {
      struct shader_info info;
      struct blob_reader reader;
      blob_reader_init(&reader, nir->data, nir->size);
      /* Keep the conservative requirement if cached metadata is incomplete. */
      return !nir_deserialize_shader_info(&reader, &info) ||
         (info.stage == MESA_SHADER_FRAGMENT &&
          (info.fs.pixel_interlock_ordered || info.fs.pixel_interlock_unordered));
   }
   /* A fast-linked library may omit its retained precompile. The compiled
    * envelope already records the requirement; do not decode or recompile it. */
   return !compiled ||
      container_of(compiled, const struct kk_shader, vk)->info.interlock_envelope;
}

static bool
kk_interlock_sample_frequency(const nir_shader *nir)
{
   if (nir->info.fs.uses_sample_shading)
      return true;
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_in)
      if (var->data.sample)
         return true;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_load_sample_id:
            case nir_intrinsic_load_sample_pos:
            case nir_intrinsic_load_barycentric_sample:
               return true;
            default:
               break;
            }
         }
      }
   }
   return false;
}

#endif
