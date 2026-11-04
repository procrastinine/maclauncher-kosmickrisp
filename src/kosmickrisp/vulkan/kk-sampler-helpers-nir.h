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
kk_sample_constant_metadata(const struct kk_descriptor_set_binding_layout *binding,
                            const struct kk_sampler *known, unsigned field,
                            bool lower, float *value)
{
   if (!known && (!binding || !binding->immutable_samplers || !binding->array_size))
      return false;
   if (!kk_sample_static_metadata(known ? known : binding->immutable_samplers[0],
                                  field, lower, value))
      return false;
   if (!known) {
      for (unsigned i = 1; i < binding->array_size; ++i) {
         float next;
         if (!kk_sample_static_metadata(binding->immutable_samplers[i], field, lower, &next) ||
             memcmp(value, &next, sizeof(*value)))
            return false;
      }
   }
   return true;
}

static bool
kk_sample_fold_metadata(nir_builder *b, nir_tex_instr *tex,
                        const struct kk_descriptor_set_binding_layout *binding,
                        const struct kk_sampler *known)
{
   unsigned field = (tex->backend_flags & KK_SAMPLE_METADATA_MASK) >>
                    KK_SAMPLE_METADATA_SHIFT;
   bool lower = tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS;
   float value;
   if (!kk_sample_constant_metadata(binding, known, field, lower, &value))
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def_replace(&tex->def, nir_imm_float(b, value));
   return true;
}

static bool kk_sample_mark(nir_builder *b, nir_tex_instr *tex, void *data);

static nir_tex_instr *
kk_sample_clone(nir_builder *b, nir_tex_instr *tex)
{
   nir_tex_instr *copy = nir_instr_as_tex(nir_instr_clone(b->shader, &tex->instr));
   copy->backend_flags &= ~(KK_SAMPLE_NATIVE_DISPATCH | KK_SAMPLE_LOWER_NATIVE_BIAS);
   nir_builder_instr_insert(b, &copy->instr);
   return copy;
}

/* The native branch uses the original operation and the ordinary texture
 * emitter. On Metal3 only the numeric native bias is needed; pointer records
 * and virtual precision stay entirely on the emulation branch. */
static nir_def *
kk_sample_native_value(nir_builder *b, nir_tex_instr *tex,
                       const struct kk_descriptor_set_binding_layout *binding,
                       const struct kk_sampler *known,
                       const struct lower_descriptors_ctx *ctx)
{
   if (!(tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS))
      return &kk_sample_clone(b, tex)->def;

   float constant_bias;
   nir_def *bias;
   bool bias_known = kk_sample_constant_metadata(binding, known,
      KK_SAMPLER_META_QUERY_BIAS, false, &constant_bias);
   if (bias_known) {
      bias = nir_imm_float16(b, constant_bias);
   } else {
      nir_def *plane = nir_get_tex_src(tex, nir_tex_src_plane);
      unsigned offset = plane ? nir_src_as_uint(nir_src_for_ssa(plane)) *
         sizeof(struct kk_sampled_image_descriptor) : 0;
      bias = load_resource_deref_desc(b, 1, 16,
         nir_get_tex_deref(tex, nir_tex_src_sampler_deref),
         offset + offsetof(struct kk_sampled_image_descriptor,
                           sampler_query_lod_bias_fp16), ctx);
   }
   if (tex->op == nir_texop_tex) {
      /* A mutable native sampler still needs its runtime Metal3 bias. One
       * implicit-LOD bias sample covers zero and nonzero values without
       * cloning the native texture operation or its resource loads. */
      nir_def *value = nir_f2f32(b, bias);
      nir_tex_instr *native = kk_sample_clone(b, tex);
      if (!bias_known || constant_bias != 0) {
         native->op = nir_texop_txb;
         nir_tex_instr_add_src(native, nir_tex_src_bias, value);
      }
      return &native->def;
   }

   nir_def *value = nir_f2f32(b, bias);
   nir_tex_instr *native = kk_sample_clone(b, tex);
   b->cursor = nir_before_instr(&native->instr);
   if (tex->op == nir_texop_txd) {
      nir_def *scale = nir_fexp2(b, value);
      nir_def *dx = nir_f2f32(b, nir_steal_tex_src(native, nir_tex_src_ddx));
      nir_def *dy = nir_f2f32(b, nir_steal_tex_src(native, nir_tex_src_ddy));
      nir_tex_instr_add_src(native, nir_tex_src_ddx, nir_fmul(b, dx, scale));
      nir_tex_instr_add_src(native, nir_tex_src_ddy, nir_fmul(b, dy, scale));
   } else {
      nir_tex_src_type source = tex->op == nir_texop_txl
                                  ? nir_tex_src_lod : nir_tex_src_bias;
      nir_def *original = nir_steal_tex_src(native, source);
      value = nir_f2f32(b, nir_fadd(b, bias, nir_f2f16(b, original)));
      nir_tex_instr_add_src(native, source, value);
   }
   b->cursor = nir_after_instr(&native->instr);
   return &native->def;
}

static bool
kk_sample_prepare_2d(nir_builder *b, nir_tex_instr *tex, unsigned flags, bool lower_native)
{
   /* Keep the software FP32-preservation lowering on its original NIR
    * operations. Hand-written Metal preparation must not bypass that ABI. */
   if (nir_is_denorm_preserve(b->shader->info.float_controls_execution_mode, 32))
      return false;
   bool implicit = tex->op == nir_texop_tex || tex->op == nir_texop_txb;
   if ((!implicit && tex->op != nir_texop_txl && tex->op != nir_texop_txd) ||
       tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_sparse ||
       (implicit && (tex->sampler_non_uniform || tex->texture_non_uniform)))
      return false;
   tex->backend_flags &= ~(KK_SAMPLE_NATIVE_DISPATCH | KK_SAMPLE_LOWER_NATIVE_BIAS);
   tex->backend_flags |= flags | KK_SAMPLE_EMULATION_ONLY | KK_SAMPLE_PREPARE_2D |
      (lower_native ? KK_SAMPLE_LOWER_NATIVE_BIAS : 0);
   return true;
}

static bool
kk_sample_native_dispatch(nir_builder *b, nir_tex_instr *tex,
                          struct kk_sample_lower_ctx *ctx,
                          const struct kk_descriptor_set_binding_layout *binding,
                          const struct kk_sampler *known, unsigned flags)
{
   bool lower_native = tex->backend_flags & KK_SAMPLE_LOWER_NATIVE_BIAS;
   if ((known && known->is_virtual) ||
       (flags & (KK_SAMPLE_MIN | KK_SAMPLE_MAX | KK_SAMPLE_BORDER_KNOWN))) {
      if (kk_sample_prepare_2d(b, tex, flags, lower_native))
         return true;
      tex->backend_flags &= ~(KK_SAMPLE_NATIVE_DISPATCH | KK_SAMPLE_LOWER_NATIVE_BIAS);
      kk_sampler_lower_bias_value(b, tex, lower_native);
      bool progress = kk_sample_mark(b, tex, ctx);
      tex->backend_flags |= KK_SAMPLE_EMULATION_ONLY;
      return progress;
   }
   b->cursor = nir_before_instr(&tex->instr);
   if (!(flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_MINMAX | KK_SAMPLE_BORDER))) {
      nir_def_replace(&tex->def,
         kk_sample_native_value(b, tex, binding, known, &ctx->descriptors));
      return true;
   }

   nir_deref_instr *deref = nir_get_tex_deref(tex, nir_tex_src_sampler_deref);
   nir_def *plane = nir_get_tex_src(tex, nir_tex_src_plane);
   unsigned offset = plane ? nir_src_as_uint(nir_src_for_ssa(plane)) *
      sizeof(struct kk_sampled_image_descriptor) : 0;
   nir_def *emulated = nir_imm_false(b);
   if (flags & (KK_SAMPLE_VIRTUAL | KK_SAMPLE_MINMAX)) {
      nir_def *metadata = load_resource_deref_desc(b, 2, 16, deref,
         offset + offsetof(struct kk_sampled_image_descriptor, sampler_reduction_mode),
         &ctx->descriptors);
      if (flags & KK_SAMPLE_VIRTUAL)
         emulated = nir_test_mask(b, nir_channel(b, metadata, 1), KK_SAMPLER_FILTER_VIRTUAL);
      if (flags & KK_SAMPLE_MINMAX)
         emulated = nir_ior(b, emulated, nir_ine_imm(b, nir_channel(b, metadata, 0), 0));
   }
   if (flags & KK_SAMPLE_BORDER) {
      nir_def *border = load_resource_deref_desc(b, 1, 16, deref,
         offset + offsetof(struct kk_sampled_image_descriptor, clamp_0_sampler_index_or_negative),
         &ctx->descriptors);
      emulated = nir_ior(b, emulated, nir_ige_imm(b, border, 0));
   }
   /* Implicit candidates have quad-uniform descriptor selection. Only this
    * uniform emulation branch needs quad derivatives or software LOD. Explicit
    * level/gradient candidates carry all their original derivative inputs. */
   emulated = nir_iand(b, nir_load_sampler_emulation_enabled_kk(b), emulated);
   nir_push_if(b, emulated);
   nir_tex_instr *slow = kk_sample_clone(b, tex);
   if (!kk_sample_prepare_2d(b, slow, flags, lower_native)) {
      kk_sampler_lower_bias_value(b, slow, lower_native);
      kk_sample_mark(b, slow, ctx);
      slow->backend_flags |= KK_SAMPLE_EMULATION_ONLY;
   }
   b->cursor = nir_after_instr(&slow->instr);
   nir_push_else(b, NULL);
   nir_def *native = kk_sample_native_value(b, tex, binding, known, &ctx->descriptors);
   nir_pop_if(b, NULL);
   nir_def_replace(&tex->def, nir_if_phi(b, &slow->def, native));
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
   if (tex->backend_flags & KK_SAMPLE_NATIVE_DISPATCH)
      return kk_sample_native_dispatch(b, tex, ctx, binding, known, flags);

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
      /* Ordinary native sampling does not consume the software LOD. Keep
       * derivatives outside the descriptor-dependent branch, but defer size
       * queries and gradient math until the same virtual/reduction predicate
       * used by the sampling helper. Sparse/query/cube-native paths consume
       * LOD independently and explicit scalar LOD needs no gradient work. */
      bool conditional_lod = !tex->is_sparse && tex->op != nir_texop_lod &&
         !(tex->backend_flags & KK_SAMPLE_NATIVE_CUBE_LOD) &&
         !nir_get_tex_src(tex, nir_tex_src_lod) &&
         !(known && known->is_virtual) &&
         !((flags & KK_SAMPLE_MINMAX) && known_mode > 0);
      nir_if *lod_if = NULL;
      if (conditional_lod) {
         nir_def *plane = nir_get_tex_src(tex, nir_tex_src_plane);
         unsigned plane_offset = plane ? nir_src_as_uint(nir_src_for_ssa(plane)) *
            sizeof(struct kk_sampled_image_descriptor) : 0;
         nir_def *metadata = load_resource_deref_desc(b, 2, 16, deref,
            plane_offset + offsetof(struct kk_sampled_image_descriptor,
                                    sampler_reduction_mode), &ctx->descriptors);
         nir_def *emulated = (flags & KK_SAMPLE_VIRTUAL) ? nir_ine_imm(b,
            nir_iand_imm(b, nir_channel(b, metadata, 1), KK_SAMPLER_FILTER_VIRTUAL), 0) :
            nir_imm_bool(b, false);
         if (flags & KK_SAMPLE_MINMAX)
            emulated = nir_ior(b, emulated,
               nir_ine_imm(b, nir_channel(b, metadata, 0), 0));
         lod_if = nir_push_if(b, emulated);
      }
      nir_def *lod = kk_minmax_lod(b, tex, dx, dy);
      if (lod_if) {
         nir_push_else(b, lod_if);
         nir_def *unused = nir_imm_float(b, 0);
         nir_pop_if(b, lod_if);
         lod = nir_if_phi(b, lod, unused);
      }
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
