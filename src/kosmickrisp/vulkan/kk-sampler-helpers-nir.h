/* Copyright 2016 Broadcom
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 * Shared sampling calls carry descriptor metadata instead of cloning footprints.
 * Included after the existing descriptor and sampler-footprint utilities.
 */
#include "../compiler/kk-sampler-helper-flags.h"
#include "util/half_float.h"
struct kk_sample_lower_ctx {
   struct lower_descriptors_ctx descriptors;
   bool minmax;
   bool custom;
};

static const struct kk_sampler *
kk_sample_static_sampler(nir_tex_instr *tex,
                         const struct kk_descriptor_set_binding_layout *binding)
{
   if (!binding || !binding->immutable_samplers)
      return NULL;
   nir_deref_instr *deref = nir_get_tex_deref(tex, nir_tex_src_sampler_deref);
   unsigned index = 0;
   if (!deref)
      return NULL;
   if (deref->deref_type == nir_deref_type_array) {
      if (!nir_src_is_const(deref->arr.index))
         return NULL;
      index = nir_src_as_uint(deref->arr.index);
   }
   return index < binding->array_size ? binding->immutable_samplers[index] : NULL;
}

/* Immutable logical state is stable for the complete pipeline lifetime. */
static bool
kk_sample_static_metadata(const struct kk_sampler *sampler, unsigned field,
                           bool lower_native_bias, float *value)
{
   const struct kk_sampler_params *params = sampler->params_cpu;
   if (!params)
      return false;
   switch (field) {
   case KK_SAMPLER_META_BIAS:
      *value = sampler->is_virtual ? params->lod_bias :
         lower_native_bias ? _mesa_half_to_float(sampler->lod_bias_fp16) : 0.0f;
      break;
   case KK_SAMPLER_META_QUERY_BIAS:
      *value = sampler->is_virtual ? params->lod_bias :
         _mesa_half_to_float(sampler->query_lod_bias_fp16);
      break;
   case KK_SAMPLER_META_MIN_LOD:
      *value = sampler->is_virtual ? params->min_lod :
         _mesa_half_to_float(sampler->lod_min_fp16);
      break;
   case KK_SAMPLER_META_MAX_LOD:
      *value = sampler->is_virtual ? params->max_lod :
         _mesa_half_to_float(sampler->lod_max_fp16);
      break;
   case KK_SAMPLER_META_MIPMAP:
      *value = (params->flags & KK_SAMPLER_PARAMS_MIPMAP_LINEAR) != 0;
      break;
   case KK_SAMPLER_META_VIRTUAL:
      *value = sampler->is_virtual;
      break;
   case KK_SAMPLER_META_MAX_ANISOTROPY:
      *value = (params->flags & KK_SAMPLER_PARAMS_ANISOTROPY_ENABLE) ?
         params->max_anisotropy : 1.0f;
      break;
   case KK_SAMPLER_META_REDUCTION_MODE:
      *value = params->reduction_mode;
      break;
   default:
      UNREACHABLE("sampler metadata selector");
   }
   return true;
}

static bool
kk_sample_fold_metadata(nir_builder *b, nir_tex_instr *tex,
                        const struct kk_descriptor_set_binding_layout *binding,
                        const struct kk_sampler *known)
{
   if (!known && (!binding || !binding->immutable_samplers || !binding->array_size))
      return false;
   unsigned field = (tex->backend_flags & KK_SAMPLE_METADATA_MASK) >>
                    KK_SAMPLE_METADATA_SHIFT;
   bool lower = tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS;
   float value;
   if (!kk_sample_static_metadata(known ? known : binding->immutable_samplers[0],
                                  field, lower, &value))
      return false;
   if (!known) {
      for (unsigned i = 1; i < binding->array_size; ++i) {
         float next;
         if (!kk_sample_static_metadata(binding->immutable_samplers[i], field, lower, &next) ||
             memcmp(&value, &next, sizeof(value)))
            return false;
      }
   }
   b->cursor = nir_before_instr(&tex->instr);
   nir_def_replace(&tex->def, nir_imm_float(b, value));
   return true;
}

static bool
kk_sample_mark(nir_builder *b, nir_tex_instr *tex, void *data)
{
   struct kk_sample_lower_ctx *ctx = data;
   if (!nir_tex_instr_need_sampler(tex))
      return false;
   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
       tex->op != nir_texop_txl && tex->op != nir_texop_txd &&
       tex->op != nir_texop_tg4 && tex->op != nir_texop_lod &&
       !(tex->backend_flags & KK_SAMPLE_METADATA))
      return false;
   nir_deref_instr *deref = nir_get_tex_deref(tex, nir_tex_src_sampler_deref);
   if (!deref)
      return false;
   nir_variable *var = nir_deref_instr_get_variable(deref);
   const struct kk_descriptor_set_binding_layout *binding = var ? get_binding_layout(
      var->data.descriptor_set, var->data.binding, &ctx->descriptors) : NULL;
   const struct kk_sampler *known = kk_sample_static_sampler(tex, binding);
   if (tex->backend_flags & KK_SAMPLE_METADATA)
      return kk_sample_fold_metadata(b, tex, binding, known);
   bool virtual_possible = !known || known->is_virtual;
   int known_mode = known ? known->reduction_mode : -1;
   int known_border = known ? known->has_border : -1;
   if (!known && binding && binding->immutable_samplers && binding->array_size) {
      virtual_possible = false;
      known_mode = binding->immutable_samplers[0]->reduction_mode;
      known_border = binding->immutable_samplers[0]->has_border;
      for (unsigned i = 0; i < binding->array_size; ++i) {
         virtual_possible |= binding->immutable_samplers[i]->is_virtual;
         if (known_mode != binding->immutable_samplers[i]->reduction_mode) known_mode = -1;
         if (known_border != binding->immutable_samplers[i]->has_border) known_border = -1;
      }
   }
   unsigned flags = KK_SAMPLE_HELPER;
   if (virtual_possible)
      flags |= KK_SAMPLE_VIRTUAL;
   if (tex->is_sparse) flags |= KK_SAMPLE_SPARSE;
   if (ctx->custom)
      flags |= KK_SAMPLE_CUSTOM;
   if (ctx->minmax && !tex->is_shadow && tex->op != nir_texop_tg4 && known_mode != 0) {
      flags |= KK_SAMPLE_MINMAX;
      if (known_mode > 0)
         flags |= known_mode == VK_SAMPLER_REDUCTION_MODE_MIN
                     ? KK_SAMPLE_MIN : KK_SAMPLE_MAX;
   }
   /* Predefined opaque-black RGBA4 needs the same correction even when the
    * optional custom-colour feature is disabled. Immutable native bindings
    * omit this path completely. */
   if (known_border != 0) {
      flags |= KK_SAMPLE_BORDER;
      if (known_border > 0)
         flags |= KK_SAMPLE_BORDER_KNOWN;
   }
   if (!(flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_MINMAX | KK_SAMPLE_BORDER |
                  KK_SAMPLE_SPARSE)) && tex->op != nir_texop_lod &&
       !(tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD))
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *dx = nir_get_tex_src(tex, nir_tex_src_ddx);
   nir_def *dy = nir_get_tex_src(tex, nir_tex_src_ddy);
   bool implicit = tex->op == nir_texop_tex || tex->op == nir_texop_txb;
   if (implicit || tex->op == nir_texop_lod) {
         /* Evaluate quad derivatives before the bound-sampler dispatch.
          * Border-only paths can diverge too, even without reduction. */
         nir_def *coord = nir_get_tex_src(tex, nir_tex_src_coord);
         nir_def *spatial = tex->is_array && tex->op != nir_texop_lod
            ? nir_trim_vector(b, coord, coord->num_components - 1) : coord;
         dx = nir_ddx(b, spatial);
         dy = nir_ddy(b, spatial);
   }
   if (tex->op != nir_texop_tg4 &&
       ((flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_MINMAX | KK_SAMPLE_SPARSE)) ||
        tex->op == nir_texop_lod ||
        (tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD))) {
      nir_def *lod = kk_minmax_lod(b, tex, dx, dy);
      nir_tex_instr_add_src(tex, nir_tex_src_backend2, nir_pad_vec4(b, lod));
   }
   if (implicit) {
         nir_def *bias = nir_get_tex_src(tex, nir_tex_src_bias);
         if (bias) {
            nir_def *scale = nir_fexp2(b, nir_f2f32(b, bias));
            dx = nir_fmul(b, dx, scale);
            dy = nir_fmul(b, dy, scale);
            nir_steal_tex_src(tex, nir_tex_src_bias);
         }
         tex->op = nir_texop_txd;
         nir_tex_instr_add_src(tex, nir_tex_src_ddx, dx);
         nir_tex_instr_add_src(tex, nir_tex_src_ddy, dy);
   }
   tex->backend_flags |= flags;
   return true;
}

bool
kk_nir_lower_sampler_helpers(nir_shader *nir, uint32_t set_layout_count,
                             struct vk_descriptor_set_layout *const *set_layouts,
                             bool minmax, bool custom)
{
   struct kk_sample_lower_ctx ctx = {.minmax = minmax, .custom = custom};
   assert(set_layout_count <= KK_MAX_SETS);
   for (unsigned i = 0; i < set_layout_count; ++i)
      if (set_layouts[i])
         ctx.descriptors.set_layouts[i] = vk_to_kk_descriptor_set_layout(set_layouts[i]);
   return nir_shader_tex_pass(nir, kk_sample_mark, nir_metadata_none, &ctx);
}

static void
kk_sample_descriptor(nir_builder *b, nir_tex_instr *tex, nir_def *sampler,
                      unsigned plane_offset_B,
                      const struct lower_descriptors_ctx *ctx)
{
   if (!(tex->backend_flags & (KK_SAMPLE_HELPER | KK_SAMPLE_METADATA)))
      return;
   nir_def *address = load_resource_addr(b, 1, 64,
      nir_src_as_deref(nir_src_for_ssa(sampler)), plane_offset_B, ctx);
   /* Preserve the full address through the float-typed backend source. */
   nir_def *backend = nir_get_tex_src(tex, nir_tex_src_backend1);
   nir_def *payload = nir_vec4(b, nir_unpack_64_2x32_split_x(b, address),
      nir_unpack_64_2x32_split_y(b, address),
      backend ? nir_channel(b, backend, 2) : nir_imm_int(b, 0),
      backend ? nir_channel(b, backend, 3) : nir_imm_int(b, 0));
   if (backend) {
      int src = nir_tex_instr_src_index(tex, nir_tex_src_backend1);
      nir_src_rewrite(&tex->src[src].src, payload);
   } else {
      nir_tex_instr_add_src(tex, nir_tex_src_backend1, payload);
   }
}
