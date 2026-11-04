/*
 * Copyright 2024 Valve Corporation
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2022-2023 Collabora Ltd. and Red Hat Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2018 Intel Corporation
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * SPDX-License-Identifier: MIT
 */
#include "kk_private.h"

#include "kk_shader.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"

#include <stdbool.h>

static nir_def *
query_min_lod(nir_builder *b, nir_tex_instr *tex, bool int_min_lod)
{
   nir_alu_type T = int_min_lod ? nir_type_uint16 : nir_type_float16;
   return nir_build_texture_query(b, tex, nir_texop_image_min_lod_agx, 1, T,
                                  false, false);
}

/* The view's minimum LOD applies under the sampler-emulation function
 * constant, which a device selects once it has an image view with a minimum
 * LOD (or a descriptor heap). Its false specialization drops the descriptor
 * load and keeps the application's operands. */
static bool
lower_min_lod(nir_builder *b, nir_tex_instr *tex, UNUSED void *data)
{
   if (nir_tex_instr_is_query(tex))
      return false;

   /* Buffer textures don't have levels-of-detail */
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF)
      return false;

   bool oob_zero = tex->op == nir_texop_txf || tex->op == nir_texop_txf_ms ||
                   tex->op == nir_texop_tg4;

   b->cursor = nir_before_instr(&tex->instr);
   nir_def *active = nir_load_sampler_emulation_enabled_kk(b);
   nir_def *min_lod = query_min_lod(b, tex, oob_zero);

   if (oob_zero) {
      /* Add bounds checking for LOD less than min LOD */
      b->cursor = nir_after_instr(&tex->instr);

      nir_def *oob;
      int lod_index = nir_tex_instr_src_index(tex, nir_tex_src_lod);
      nir_src *lod = lod_index >= 0 ? &tex->src[lod_index].src : NULL;
      if (lod && (!nir_src_is_const(*lod) || nir_src_as_uint(*lod) != 0)) {
         /* Out of these ops, only txf supports non-zero LOD */
         assert(tex->op == nir_texop_txf);
         oob = nir_ult(b, nir_u2uN(b, lod->ssa, min_lod->bit_size), min_lod);
      } else {
         /* LOD is 0, so out-of-bounds if min LOD is not also 0 */
         oob = nir_ine_imm(b, min_lod, 0);
      }

      nir_def *old = &tex->def;
      nir_def *zero = nir_imm_zero(b, old->num_components, old->bit_size);
      nir_def *new_ = nir_bcsel(b, nir_iand(b, active, oob), zero, old);
      nir_def_rewrite_uses_after(old, new_);
   } else {
      /* Clamp min LOD value, or clamp LOD value if explicit LOD */
      nir_tex_src_type other_src =
         tex->op == nir_texop_txl ? nir_tex_src_lod : nir_tex_src_min_lod;
      nir_def *other = nir_steal_tex_src(tex, other_src);
      nir_def *value;
      if (other) {
         nir_def *clamped = nir_fmax(b, nir_f2fN(b, other, min_lod->bit_size), min_lod);
         value = nir_bcsel(b, active, nir_f2fN(b, clamped, other->bit_size), other);
      } else {
         value = nir_bcsel(b, active, min_lod, nir_imm_floatN_t(b, 0.0, min_lod->bit_size));
      }
      nir_tex_instr_add_src(tex, other_src, value);
   }

   return true;
}

bool
kk_nir_lower_image_view_min_lod(nir_shader *nir)
{
   return nir_shader_tex_pass(nir, lower_min_lod, nir_metadata_none, NULL);
}
