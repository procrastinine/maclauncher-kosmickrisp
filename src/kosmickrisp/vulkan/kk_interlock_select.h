/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_SELECT_H
#define KK_INTERLOCK_SELECT_H

/* Linking gives the producer compiler the fragment execution requirements,
 * including requirements introduced by ordinary blend/input lowering. */
static bool
kk_interlock_link_graphics(struct vk_device *device)
{
   return device->enabled_features.fragmentShaderPixelInterlock;
}

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
