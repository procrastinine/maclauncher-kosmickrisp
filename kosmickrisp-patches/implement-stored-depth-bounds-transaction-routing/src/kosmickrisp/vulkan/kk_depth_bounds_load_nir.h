/* SPDX-License-Identifier: MIT */
#ifndef KK_DEPTH_BOUNDS_LOAD_NIR_H
#define KK_DEPTH_BOUNDS_LOAD_NIR_H

/* Same stored-attachment read for single-sample and multisample arrays. A
 * one-sample view is not a multisample Metal texture. The runtime branch and
 * its opaque handle retain the actual attachment's sample/type identity. */
static nir_def *
kk_depth_bounds_attachment_fetch(nir_builder *b, nir_def *params,
   nir_def *address, nir_def *pixel, nir_def *sample, nir_alu_type type,
   bool depth, bool multisample)
{
   enum glsl_sampler_dim dim = multisample ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D;
   nir_def *handle = depth ? nir_load_depth_texture_kk(b, 1, 64, address,
      .image_dim = dim, .image_array = true) :
      nir_load_texture_handle_kk(b, 1, 64, address, .image_dim = dim,
         .image_array = true, .dest_type = type, .flags = MSL_ACCESS_READ);
   nir_tex_instr *tex = nir_tex_instr_create(b->shader, 3);
   tex->op = multisample ? nir_texop_txf_ms : nir_texop_txf;
   tex->sampler_dim = dim;
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
   tex->src[2] = (nir_tex_src){
      .src_type = multisample ? nir_tex_src_ms_index : nir_tex_src_lod,
      .src = nir_src_for_ssa(multisample ? sample : nir_imm_int(b, 0))};
   nir_def_init(&tex->instr, &tex->def, depth ? 1 : 4, 32);
   nir_builder_instr_insert(b, &tex->instr);
   return &tex->def;
}

static nir_def *
kk_depth_bounds_attachment_read(nir_builder *b, nir_def *params,
   unsigned offset, nir_def *pixel, nir_def *sample, nir_alu_type type,
   bool depth)
{
   nir_def *address = nir_iadd_imm(b, params, offset);
   nir_push_if(b, nir_ine_imm(b,
      nir_load_global_constant(b, 1, 64, address, .align_mul = 8), 0));
   nir_push_if(b, nir_ugt_imm(b, kk_interlock_env(b, params, samples), 1));
   nir_def *ms = kk_depth_bounds_attachment_fetch(b, params, address,
      pixel, sample, type, depth, true);
   nir_push_else(b, NULL);
   nir_def *ss = kk_depth_bounds_attachment_fetch(b, params, address,
      pixel, sample, type, depth, false);
   nir_pop_if(b, NULL);
   nir_def *stored = nir_if_phi(b, ms, ss);
   nir_push_else(b, NULL);
   /* No attachment: the state producer disables its DS tests. These neutral
    * unused shadow words never stand in for an unavailable real attachment;
    * a failed view creation returns an error before this program is run. */
   nir_def *absent = depth ? nir_imm_float(b, 1) : nir_imm_ivec4(b, 0, 0, 0, 0);
   nir_pop_if(b, NULL);
   return nir_if_phi(b, stored, absent);
}
#endif
