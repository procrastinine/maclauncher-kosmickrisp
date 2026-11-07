/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BOUNDS_CLEAR_NIR_H
#define KK_DEPTH_BOUNDS_CLEAR_NIR_H
/* Initialize only capture claims/coverage, not the current DS/color shadow.
 * Prefix -> existing continuation -> next prefix orders reuse of one finite
 * workspace. Host memset during recording cannot provide that ordering. */
static nir_shader *
kk_depth_bounds_clear_shader(const nir_shader_compiler_options *options,
   struct kk_interlock_render_nir *render)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      options, "depth bounds capture clear");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   nir_def *params = render->parameters(&b, render->data);
   nir_def *cell = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *cells = nir_imul(&b, kk_interlock_env(&b, params, tile_width),
      nir_imul(&b, kk_interlock_env(&b, params, tile_height),
         kk_interlock_env(&b, params, primitive_count)));
   nir_push_if(&b, nir_ult(&b, cell, cells));
   nir_store_global(&b, nir_imm_int(&b, 0), kk_interlock_word_address(&b,
      kk_interlock_env(&b, params, claims), cell), .align_mul = 4);
   nir_store_global(&b, nir_imm_int(&b, 0), kk_interlock_word_address(&b,
      kk_interlock_env(&b, params, coverage), cell), .align_mul = 4);
   nir_def *metadata = nir_iadd(&b, kk_interlock_env(&b, params, metadata),
      nir_u2u64(&b, nir_imul_imm(&b, cell, 16)));
   nir_store_global(&b, nir_imm_ivec4(&b, 0, 0, 0, 0), metadata, .align_mul = 16);
   nir_pop_if(&b, NULL);
   return b.shader;
}
#endif
