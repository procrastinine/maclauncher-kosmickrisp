/* SPDX-License-Identifier: MIT
 * Cube projection follows the Vulkan cube face coordinate table. Face-local
 * quotient-rule gradients preserve mip selection and anisotropic footprints.
 */
#ifndef KK_NON_SEAMLESS_CUBE_H
#define KK_NON_SEAMLESS_CUBE_H

static nir_def *
kk_cube_face_components(nir_builder *b, nir_def *coord, nir_def *face)
{
   nir_def *x = nir_channel(b, coord, 0);
   nir_def *y = nir_channel(b, coord, 1);
   nir_def *z = nir_channel(b, coord, 2);
   nir_def *nx = nir_fneg(b, x), *ny = nir_fneg(b, y), *nz = nir_fneg(b, z);
   nir_def *components[] = {
      nir_vec3(b, nz, ny, x), nir_vec3(b, z, ny, nx),
      nir_vec3(b, x, z, y), nir_vec3(b, x, nz, ny),
      nir_vec3(b, x, ny, z), nir_vec3(b, nx, ny, nz),
   };
   nir_def *value = components[5];
   for (int i = 4; i >= 0; --i)
      value = nir_bcsel(b, nir_ieq_imm(b, face, i), components[i], value);
   return value;
}

static nir_def *
kk_cube_face_gradient(nir_builder *b, nir_def *coord, nir_def *grad, nir_def *face)
{
   nir_def *p = kk_cube_face_components(b, coord, face);
   nir_def *d = kk_cube_face_components(b, grad, face);
   nir_def *rcp = nir_frcp(b, nir_channel(b, p, 2));
   nir_def *quotient = nir_fmul(b, nir_trim_vector(b, p, 2), rcp);
   return nir_fmul(b, nir_fsub(b, nir_trim_vector(b, d, 2),
      nir_fmul(b, quotient, nir_channel(b, d, 2))), nir_fmul_imm(b, rcp, 0.5));
}

static bool
kk_lower_non_seamless_cube(nir_builder *b, nir_tex_instr *tex, void *data)
{
   const struct lower_descriptors_ctx *ctx = data;
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE ||
       (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
        tex->op != nir_texop_txl && tex->op != nir_texop_txd && tex->op != nir_texop_tg4))
      return false;
   nir_def *sampler_def = nir_get_tex_src(tex, nir_tex_src_sampler_deref);
   if (!sampler_def)
      return false;
   nir_deref_instr *sampler = nir_src_as_deref(nir_src_for_ssa(sampler_def));
   nir_variable *var = nir_deref_instr_get_variable(sampler);
   const struct kk_descriptor_set_binding_layout *binding =
      var ? get_binding_layout(var->data.descriptor_set, var->data.binding, ctx) : NULL;
   /* Immutable bindings specialize this operation at compilation. A mixed
    * dynamically indexed array still needs the runtime sampler metadata. */
   int non_seamless = -1;
   if (binding && binding->immutable_samplers) {
      if (sampler->deref_type == nir_deref_type_array &&
          nir_deref_instr_parent(sampler)->deref_type == nir_deref_type_var &&
          nir_src_is_const(sampler->arr.index)) {
         unsigned index = nir_src_as_uint(sampler->arr.index);
         if (index < binding->array_size)
            non_seamless = !!(binding->immutable_samplers[index]->filter_bits &
               KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE);
      }
      if (non_seamless < 0) {
         bool any = false, all = true;
         for (unsigned i = 0; i < binding->array_size; ++i) {
            bool flag = binding->immutable_samplers[i]->filter_bits &
               KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE;
            any |= flag;
            all &= flag;
            /* A mixed dynamic binding is already fully classified. */
            if (any && !all) break;
         }
         if (!any) return false;
         if (all) non_seamless = 1;
      }
      if (non_seamless == 0) return false;
   }
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
   nir_def *dx = nir_get_tex_src(tex, nir_tex_src_ddx);
   nir_def *dy = nir_get_tex_src(tex, nir_tex_src_ddy);
   bool implicit = tex->op == nir_texop_tex || tex->op == nir_texop_txb;
   bool dynamic_sampler = non_seamless < 0;
   nir_tex_instr *seamless = NULL;
   if (dynamic_sampler) {
      /* Implicit native sampling stays outside all control flow. The outer
       * condition is a uniform Metal function constant, so derivatives inside
       * it remain uniform and disappear entirely in the native specialization. */
      if (implicit) {
         seamless = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
         nir_builder_instr_insert(b, &seamless->instr);
      }
      nir_push_if(b, nir_load_sampler_emulation_enabled_kk(b));
   }
   if (implicit) {
      // Derivatives must be taken before the sampler-dependent branch.
      dx = nir_ddx(b, nir_trim_vector(b, coord, 3));
      dy = nir_ddy(b, nir_trim_vector(b, coord, 3));
      nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
      if (bias) {
         nir_def *scale = nir_fexp2(b, nir_f2f32(b, bias));
         dx = nir_fmul(b, dx, scale);
         dy = nir_fmul(b, dy, scale);
      }
   }
   nir_def *flags = dynamic_sampler ?
      load_resource_deref_desc(b, 1, 16, sampler,
         offsetof(struct kk_sampled_image_descriptor, sampler_filter_bits), ctx) : NULL;
   if (dynamic_sampler)
      nir_push_if(b, nir_ine_imm(b,
         nir_iand_imm(b, flags, KK_SAMPLER_FILTER_NON_SEAMLESS_CUBE), 0));
   nir_def *face;
   nir_def *uv = nir_fmul_imm(b, kk_minmax_cube_uv(b, coord, &face), 0.5);
   nir_def *layer = nir_i2f32(b, face);
   if (tex->is_array) {
      nir_def *size = nir_build_texture_query(b, tex, nir_texop_txs, 3,
         nir_type_uint32, false, true);
      nir_def *array_layer = nir_fround_even(b, nir_channel(b, coord, 3));
      array_layer = nir_fmin(b, nir_fmax(b, array_layer, nir_imm_float(b, 0)),
         nir_u2f32(b, nir_iadd_imm(b, nir_channel(b, size, 2), -1)));
      layer = nir_fadd(b, layer, nir_fmul_imm(b, array_layer, 6));
   }
   nir_tex_instr *sample = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   sample->sampler_dim = GLSL_SAMPLER_DIM_2D;
   sample->is_array = true;
   sample->coord_components = 3;
   nir_builder_instr_insert(b, &sample->instr);
   b->cursor = nir_before_instr(&sample->instr);
   int ci = nir_tex_instr_src_index(sample, nir_tex_src_coord);
   nir_src_rewrite(&sample->src[ci].src,
      nir_vec3(b, nir_channel(b, uv, 0), nir_channel(b, uv, 1), layer));
   if (implicit) {
      nir_steal_tex_src(sample, nir_tex_src_bias);
      sample->op = nir_texop_txd;
      nir_tex_instr_add_src(sample, nir_tex_src_ddx, dx);
      nir_tex_instr_add_src(sample, nir_tex_src_ddy, dy);
   }
   if (dx) {
      int xi = nir_tex_instr_src_index(sample, nir_tex_src_ddx);
      int yi = nir_tex_instr_src_index(sample, nir_tex_src_ddy);
      nir_src_rewrite(&sample->src[xi].src, kk_cube_face_gradient(b, coord, dx, face));
      nir_src_rewrite(&sample->src[yi].src, kk_cube_face_gradient(b, coord, dy, face));
   }
   b->cursor = nir_after_instr(&sample->instr);
   nir_def *result = &sample->def;
   if (dynamic_sampler) {
      nir_push_else(b, NULL);
      if (!implicit) {
         seamless = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
         nir_builder_instr_insert(b, &seamless->instr);
      }
      nir_pop_if(b, NULL);
      result = nir_if_phi(b, &sample->def, &seamless->def);
      nir_push_else(b, NULL);
      if (!implicit) {
         seamless = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
         nir_builder_instr_insert(b, &seamless->instr);
      }
      nir_pop_if(b, NULL);
      result = nir_if_phi(b, result, &seamless->def);
   }
   nir_def_replace(&tex->def, result);
   return true;
}

bool
kk_nir_lower_non_seamless_cube(nir_shader *nir, uint32_t set_layout_count,
                              struct vk_descriptor_set_layout *const *set_layouts)
{
   struct lower_descriptors_ctx ctx = {};
   assert(set_layout_count <= KK_MAX_SETS);
   for (unsigned i = 0; i < set_layout_count; ++i)
      if (set_layouts[i])
         ctx.set_layouts[i] = vk_to_kk_descriptor_set_layout(set_layouts[i]);
   return nir_shader_tex_pass(nir, kk_lower_non_seamless_cube, nir_metadata_none, &ctx);
}
#endif
