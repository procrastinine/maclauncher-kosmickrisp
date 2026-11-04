/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_REPLAY_NIR_H
#define KK_INTERLOCK_REPLAY_NIR_H

#include "kk_interlock_render_nir.h"
#include "util/format/u_format.h"

/* The viewport is the aligned tile. Three synthetic vertices cover it; the
 * primitive number selects a captured raster primitive in transaction draws.
 * No application vertex or geometry function is called by either pass. */
static nir_shader *
kk_interlock_tile_vertex_shader(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      options, "interlock tile rasterization");
   b.shader->info.io_lowered = true;
   nir_def *corner = nir_umod_imm(&b, nir_load_vertex_id(&b), 3);
   nir_def *x = nir_bcsel(&b, nir_ieq_imm(&b, corner, 1),
      nir_imm_float(&b, 3), nir_imm_float(&b, -1));
   nir_def *y = nir_bcsel(&b, nir_ieq_imm(&b, corner, 2),
      nir_imm_float(&b, 3), nir_imm_float(&b, -1));
   nir_store_output(&b, nir_vec4(&b, x, y,
      nir_imm_float(&b, 0), nir_imm_float(&b, 1)), nir_imm_int(&b, 0),
      .src_type = nir_type_float32,
      .io_semantics = {.location = VARYING_SLOT_POS, .num_slots = 1},
      .write_mask = 15);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

struct kk_interlock_replay_info {
   enum pipe_format color_format[8];
   bool depth;
   bool stencil;
};

/* Replay is sample-frequency and uses a plain replacement pipeline: source
 * fetch/blend, alpha coverage, output masks and depth/stencil tests have already
 * updated the tile shadow. Replay performs no source storage side effects and
 * never contributes native occlusion samples. */
static nir_shader *
kk_interlock_replay_shader(const nir_shader_compiler_options *options,
   struct kk_interlock_render_nir *render,
   const struct kk_interlock_replay_info *info)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      options, "interlock attachment replay");
   b.shader->info.io_lowered = true;
   b.shader->info.fs.uses_sample_shading = true;
   b.shader->info.fs.depth_layout = info->depth ? FRAG_DEPTH_LAYOUT_ANY : FRAG_DEPTH_LAYOUT_NONE;
   nir_def *params = render->parameters(&b, render->data);
   nir_def *pixel = nir_f2u32(&b, nir_channels(&b, nir_load_frag_coord(&b), 3));
   nir_def *cell = kk_interlock_cell(&b, params, pixel, nir_imm_int(&b, 0));
   nir_def *index = nir_iadd(&b, nir_imul(&b, cell,
      kk_interlock_env(&b, params, samples)), nir_load_sample_id(&b));
   nir_def *zero = nir_imm_int(&b, 0);
   for (unsigned i = 0; i < ARRAY_SIZE(info->color_format); ++i) {
      enum pipe_format format = info->color_format[i];
      if (format == PIPE_FORMAT_NONE)
         continue;
      nir_def *base = kk_interlock_env_load(&b, params, 1, 64,
         offsetof(struct kk_interlock_envelope, color) + i * sizeof(uint64_t));
      nir_def *address = nir_iadd(&b, base,
         nir_u2u64(&b, nir_imul_imm(&b, index, 16)));
      nir_def *value = nir_load_global_constant(&b, 4, 32, address,
                                                 .align_mul = 16);
      nir_alu_type type = util_format_is_pure_uint(format) ? nir_type_uint32 :
         util_format_is_pure_sint(format) ? nir_type_int32 : nir_type_float32;
      unsigned components = util_format_get_nr_components(format);
      nir_store_output(&b, nir_trim_vector(&b, value, components), zero, .src_type = type,
         .io_semantics = {.location = FRAG_RESULT_DATA0 + i, .num_slots = 1},
         .write_mask = BITFIELD_MASK(components));
   }
   if (info->depth) {
      nir_def *address = kk_interlock_word_address(&b,
         kk_interlock_env(&b, params, depth), index);
      nir_store_output(&b,
         nir_load_global_constant(&b, 1, 32, address, .align_mul = 4), zero,
         .src_type = nir_type_float32,
         .io_semantics = {.location = FRAG_RESULT_DEPTH, .num_slots = 1},
         .write_mask = 1);
   }
   if (info->stencil) {
      nir_def *address = kk_interlock_word_address(&b,
         kk_interlock_env(&b, params, stencil), index);
      nir_store_output(&b,
         nir_load_global_constant(&b, 1, 32, address, .align_mul = 4), zero,
         .src_type = nir_type_uint32,
         .io_semantics = {.location = FRAG_RESULT_STENCIL, .num_slots = 1},
         .write_mask = 1);
   }
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

#endif
