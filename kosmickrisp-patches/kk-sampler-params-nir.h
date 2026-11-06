/* SPDX-License-Identifier: MIT */
#ifndef KK_SAMPLER_PARAMS_NIR_H
#define KK_SAMPLER_PARAMS_NIR_H
#include "nir.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"
#include "kk-sampler-helper-flags.h"

/* Keep metadata queries in the texture IR until descriptor addresses are
 * known. The MSL emitter resolves every virtual value through the one
 * complete-metadata accessor; pointer chunks are never numeric half values. */


static nir_def *
kk_sampler_query_metadata(nir_builder *b, nir_tex_instr *tex,
                          enum kk_sampler_metadata_value field,
                          bool lower_native_bias)
{
   nir_def *value = nir_build_texture_query(b, tex, nir_texop_lod_bias,
                                            1, nir_type_float32, false, false);
   nir_tex_instr *query = nir_instr_as_tex(nir_def_instr(value));
   nir_def *plane = nir_get_tex_src(tex, nir_tex_src_plane);
   if (plane) nir_tex_instr_add_src(query, nir_tex_src_plane, plane);
   query->backend_flags = KK_SAMPLE_METADATA | (field << KK_SAMPLE_METADATA_SHIFT) |
      (lower_native_bias ? KK_SAMPLE_LOWER_NATIVE_BIAS : 0);
   return value;
}

static bool
kk_sampler_lower_bias_instr(nir_builder *b, nir_tex_instr *tex, void *data)
{
   if ((tex->backend_flags & KK_SAMPLE_METADATA) ||
       !nir_tex_instr_need_sampler(tex) || tex->op == nir_texop_tg4)
      return false;
   bool lower_native = *(bool *)data;
   /* Preserve the original explicit cube form. Native sampling keeps its
    * upstream scalar level while virtual sampling still has its gradients. */
   if (tex->op == nir_texop_txd && tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE)
      tex->backend_flags |= KK_SAMPLE_NATIVE_CUBE_LOD;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *bias = kk_sampler_query_metadata(b, tex, KK_SAMPLER_META_BIAS,
                                            lower_native);
   if (tex->op == nir_texop_txd) {
      nir_def *dx = nir_f2f32(b, nir_steal_tex_src(tex, nir_tex_src_ddx));
      nir_def *dy = nir_f2f32(b, nir_steal_tex_src(tex, nir_tex_src_ddy));
      nir_def *scale = nir_fexp2(b, bias);
      nir_tex_instr_add_src(tex, nir_tex_src_ddx, nir_fmul(b, dx, scale));
      nir_tex_instr_add_src(tex, nir_tex_src_ddy, nir_fmul(b, dy, scale));
   } else if (tex->op == nir_texop_tex) {
      tex->op = nir_texop_txb;
      nir_tex_instr_add_src(tex, nir_tex_src_bias, bias);
   } else {
      nir_tex_src_type source = tex->op == nir_texop_txl
                                 ? nir_tex_src_lod : nir_tex_src_bias;
      nir_def *original = nir_steal_tex_src(tex, source);
      if (original) {
         nir_def *exact = nir_fadd(b, bias, nir_f2f32(b, original));
         /* Match the upstream half addition on native Metal3 samplers. The
          * virtual path retains original fp32 operands, with no double bias.
          * Metal4 native samplers already apply their own physical bias. */
         if (lower_native) {
            nir_def *native = nir_f2f32(b, nir_fadd(b, nir_f2f16(b, bias),
                                                        nir_f2f16(b, original)));
            nir_def *virt = kk_sampler_query_metadata(b, tex,
               KK_SAMPLER_META_VIRTUAL, false);
            bias = nir_bcsel(b, nir_fneu_imm(b, virt, 0), exact, native);
         } else {
            bias = exact;
         }
      }
      nir_tex_instr_add_src(tex, source, bias);
   }
   return true;
}

static bool
kk_sampler_lower_bias(nir_shader *nir, bool lower_native_bias)
{
   return nir_shader_tex_pass(nir, kk_sampler_lower_bias_instr,
                              nir_metadata_control_flow, &lower_native_bias);
}
#endif
