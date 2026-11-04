/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_CLEAR_NIR_H
#define KK_FRAGMENT_SHADING_RATE_CLEAR_NIR_H

/* A selected capture slot is reusable only after its previous readers finish.
 * This program clears capture data; persistent DS/color shadows are untouched.
 * The phase owner supplies the existing encoder/continuation ordering. */
struct kk_vrs_clear_nir {
   nir_def *valid, *pixels, *words;
   nir_def *inputs, *claims, *coverage, *metadata;
};

static struct kk_vrs_clear_nir
kk_vrs_clear_begin(nir_builder *b)
{
   nir_def *context = nir_load_per_draw_ptr_kk(b, 1, 64);
   nir_def *env = kk_vrs_cells_parameters(b, NULL);
   nir_def *slot = kk_interlock_env_load(b, context, 1, 32,
      offsetof(struct kk_vrs_transaction_context, capture_primitive));
   nir_def *width = kk_interlock_env(b, env, tile_width);
   nir_def *height = kk_interlock_env(b, env, tile_height);
   nir_def *samples = kk_interlock_env(b, env, samples);
   nir_def *stride = kk_interlock_env(b, env, input_stride);
   nir_def *pixels = nir_imul(b, width, height);
   nir_def *words = nir_imul(b, nir_imul(b, pixels, samples), nir_ushr_imm(b, stride, 2));
   nir_def *valid = nir_iand(b,
      nir_ult(b, slot, kk_interlock_env(b, env, primitive_count)),
      nir_iand(b, nir_ieq(b, width, height),
         nir_iand(b, nir_uge_imm(b, width, 8), nir_uge(b, nir_imm_int(b, 32), width))));
   valid = nir_iand(b, valid, nir_ieq_imm(b, nir_iand(b, width, nir_iadd_imm(b, width, -1)), 0));
   valid = nir_iand(b, valid, nir_ior(b, nir_ieq_imm(b, samples, 1),
      nir_ior(b, nir_ieq_imm(b, samples, 2), nir_ieq_imm(b, samples, 4))));
   valid = nir_iand(b, valid, nir_iand(b,
      nir_uge_imm(b, stride, 64), nir_uge(b, nir_imm_int(b, ALIGN_POT(UINT16_MAX + 96u, 16)), stride)));
   valid = nir_iand(b, valid, nir_ieq_imm(b, nir_iand_imm(b, stride, 15), 0));
   nir_def *cell = nir_imul(b, slot, pixels);
   return (struct kk_vrs_clear_nir){
      .valid = valid, .pixels = pixels, .words = words,
      .inputs = nir_iadd(b, kk_interlock_env(b, env, inputs),
         nir_imul_imm(b, nir_imul(b, nir_u2u64(b, slot), nir_u2u64(b, words)), 4)),
      .claims = kk_interlock_word_address(b, kk_interlock_env(b, env, claims), cell),
      .coverage = kk_interlock_word_address(b, kk_interlock_env(b, env, coverage), cell),
      .metadata = nir_iadd(b, kk_interlock_env(b, env, metadata),
         nir_imul_imm(b, nir_u2u64(b, cell), 16)),
   };
}

static void
kk_vrs_clear_word(nir_builder *b, const struct kk_vrs_clear_nir *s, nir_def *word)
{
   nir_push_if(b, nir_ult(b, word, s->words));
   nir_store_global(b, nir_imm_int(b, 0),
      kk_interlock_word_address(b, s->inputs, word), .write_mask = 1, .align_mul = 4);
   nir_push_if(b, nir_ult(b, word, s->pixels));
   nir_store_global(b, nir_imm_int(b, 0),
      kk_interlock_word_address(b, s->claims, word), .write_mask = 1, .align_mul = 4);
   nir_store_global(b, nir_imm_int(b, 0),
      kk_interlock_word_address(b, s->coverage, word), .write_mask = 1, .align_mul = 4);
   nir_store_global(b, nir_imm_ivec4(b, 0, 0, 0, 0),
      nir_iadd(b, s->metadata, nir_imul_imm(b, nir_u2u64(b, word), 16)),
      .write_mask = 15, .align_mul = 16);
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
}

static nir_shader *
kk_vrs_clear_shader(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
      "bounded VRS capture-slot clear");
   b.shader->info.workgroup_size[0] = 256;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   struct kk_vrs_clear_nir state = kk_vrs_clear_begin(&b);
   nir_push_if(&b, state.valid);
   nir_variable *word = nir_local_variable_create(b.impl, glsl_uint_type(), "capture_word");
   nir_store_var(&b, word, nir_load_local_invocation_index(&b), 1);
   nir_push_loop(&b);
   nir_def *index = nir_load_var(&b, word);
   nir_push_if(&b, nir_uge(&b, index, state.words));
   nir_jump(&b, nir_jump_break);
   nir_pop_if(&b, NULL);
   kk_vrs_clear_word(&b, &state, index);
   nir_store_var(&b, word, nir_iadd_imm(&b, index, 256), 1);
   nir_pop_loop(&b, NULL);
   nir_pop_if(&b, NULL);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}
#endif
