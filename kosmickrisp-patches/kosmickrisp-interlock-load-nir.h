/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_LOAD_NIR_H
#define KK_INTERLOCK_LOAD_NIR_H

#include "kk_interlock_replay_nir.h"
#include "kosmickrisp/compiler/nir_to_msl.h"

/* These views are always two-dimensional multisample arrays, including for a
 * single framebuffer layer. The native packet creates aspect-specific views
 * of the stored attachments. No same-pass depth/stencil access is required. */
static nir_def *
kk_interlock_attachment_read(nir_builder *b, nir_def *params,
   unsigned offset, nir_def *pixel, nir_def *sample, nir_alu_type type,
   bool depth)
{
   nir_def *address = nir_iadd_imm(b, params, offset);
   nir_push_if(b, nir_ine_imm(b,
      nir_load_global_constant(b, 1, 64, address, .align_mul = 8), 0));
   nir_def *handle = depth ? nir_load_depth_texture_kk(b, 1, 64, address,
      .image_dim = GLSL_SAMPLER_DIM_MS, .image_array = true) :
      nir_load_texture_handle_kk(b, 1, 64, address,
         .image_dim = GLSL_SAMPLER_DIM_MS, .image_array = true,
         .dest_type = type, .flags = MSL_ACCESS_READ);
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 3);
   tex->op = nir_texop_txf_ms;
   tex->sampler_dim = GLSL_SAMPLER_DIM_MS;
   tex->is_array = true;
   tex->is_shadow = depth;
   tex->is_new_style_shadow = depth;
   tex->coord_components = 3;
   tex->dest_type = type;
   tex->src[0] = (nir_tex_src){.src_type = nir_tex_src_texture_handle,
                              .src = nir_src_for_ssa(handle)};
   tex->src[1] = (nir_tex_src){.src_type = nir_tex_src_coord,
      .src = nir_src_for_ssa(nir_vec3(b, nir_channel(b, pixel, 0),
         nir_channel(b, pixel, 1), kk_interlock_env(b, params, layer)))};
   tex->src[2] = (nir_tex_src){.src_type = nir_tex_src_ms_index,
                              .src = nir_src_for_ssa(sample)};
   nir_def_init(&tex->instr, &tex->def, depth ? 1 : 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   nir_push_else(b, NULL);
   nir_def *absent = depth ? nir_imm_float(b, 1) : nir_imm_ivec4(b, 0, 0, 0, 0);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, &tex->def, absent);
}

static nir_shader *
kk_interlock_load_shader(const nir_shader_compiler_options *options,
   struct kk_interlock_render_nir *render,
   const struct kk_interlock_replay_info *info)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      options, "interlock attachment load");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   nir_def *params = render->parameters(&b, render->data);
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *local = nir_channels(&b, id, 3);
   nir_def *sample = nir_channel(&b, id, 2);
   nir_def *width = kk_interlock_env(&b, params, tile_width);
   nir_def *height = kk_interlock_env(&b, params, tile_height);
   nir_def *samples = kk_interlock_env(&b, params, samples);
   nir_push_if(&b, nir_iand(&b,
      nir_iand(&b, nir_ult(&b, nir_channel(&b, local, 0), width),
                   nir_ult(&b, nir_channel(&b, local, 1), height)),
      nir_ult(&b, sample, samples)));
   nir_def *pixel = nir_iadd(&b, local, nir_vec2(&b,
      kk_interlock_env(&b, params, tile_x),
      kk_interlock_env(&b, params, tile_y)));
   nir_push_if(&b, nir_iand(&b,
      nir_ult(&b, nir_channel(&b, pixel, 0), kk_interlock_env(&b, params, width)),
      nir_ult(&b, nir_channel(&b, pixel, 1), kk_interlock_env(&b, params, height))));
   nir_def *index = nir_iadd(&b, nir_imul(&b,
      nir_iadd(&b, nir_imul(&b, nir_channel(&b, local, 1), width),
         nir_channel(&b, local, 0)), samples), sample);
   for (unsigned i = 0; i < ARRAY_SIZE(info->color_format); ++i) {
      enum pipe_format format = info->color_format[i];
      if (format == PIPE_FORMAT_NONE)
         continue;
      nir_alu_type type = util_format_is_pure_uint(format) ? nir_type_uint32 :
         util_format_is_pure_sint(format) ? nir_type_int32 : nir_type_float32;
      nir_def *value = kk_interlock_attachment_read(&b, params,
         offsetof(struct kk_interlock_envelope, color_texture) + i * 8,
         pixel, sample, type, false);
      nir_def *base = kk_interlock_env_load(&b, params, 1, 64,
         offsetof(struct kk_interlock_envelope, color) + i * 8);
      nir_def *address = nir_iadd(&b, base,
         nir_u2u64(&b, nir_imul_imm(&b, index, 16)));
      nir_store_global(&b, value, address, .align_mul = 16);
   }
   nir_def *depth = info->depth ? kk_interlock_attachment_read(&b, params,
      offsetof(struct kk_interlock_envelope, depth_texture), pixel, sample,
      nir_type_float32, true) : nir_imm_float(&b, 1);
   nir_def *stencil = info->stencil ? nir_channel(&b,
      kk_interlock_attachment_read(&b, params,
         offsetof(struct kk_interlock_envelope, stencil_texture), pixel, sample,
         nir_type_uint32, false), 0) : nir_imm_int(&b, 0);
   nir_store_global(&b, depth, kk_interlock_word_address(&b,
      kk_interlock_env(&b, params, depth), index), .align_mul = 4);
   nir_store_global(&b, stencil, kk_interlock_word_address(&b,
      kk_interlock_env(&b, params, stencil), index), .align_mul = 4);
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

#endif
