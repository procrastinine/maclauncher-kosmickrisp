/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_FRAMEBUFFER_NIR_H
#define KK_INTERLOCK_FRAMEBUFFER_NIR_H

#include "kk_interlock_render_nir.h"
#include "kk_interlock_tests_nir.h"
#include "compiler/nir/nir_format_convert.h"

struct kk_interlock_framebuffer_nir {
   struct kk_interlock_transaction_nir transaction;
   uint64_t outputs;
   enum pipe_format color_format[8];
   unsigned depth_bits;
   bool early;
   bool post_depth_coverage;
   bool stencil_export;
   nir_variable *tested_coverage;
   nir_variable *sample_outputs;
   nir_function *sample_function;
   uint64_t dual_outputs;
};

static nir_def *
kk_interlock_shadow_sample(nir_builder *b,
   struct kk_interlock_transaction_nir *s, nir_def *params, nir_def *sample)
{
   nir_def *pixel = nir_load_var(b, s->pixel);
   nir_def *cell = kk_interlock_cell(b, params, pixel, nir_imm_int(b, 0));
   return nir_iadd(b, nir_imul(b, cell,
      kk_interlock_env(b, params, samples)), sample);
}

static nir_def *
kk_interlock_shadow_address(nir_builder *b, nir_def *params,
   unsigned pointer_offset, nir_def *sample, unsigned stride)
{
   nir_def *base = kk_interlock_env_load(b, params, 1, 64, pointer_offset);
   return nir_iadd(b, base,
      nir_u2u64(b, nir_imul_imm(b, sample, stride)));
}

/* Source attachment reads retain their explicit sample operand. They cannot
 * use the representative source-invocation sample, nor the fixed-function
 * destination bank used while blending each current output sample. */
static nir_def *
kk_interlock_framebuffer_load_attachment(nir_builder *b,
   struct kk_interlock_program_nir *program, nir_intrinsic_instr *intr)
{
   nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   if (io.location < FRAG_RESULT_DATA0 ||
       io.location >= FRAG_RESULT_DATA0 + 8)
      return NULL;
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *slot = nir_iadd_imm(b, intr->src[0].ssa,
      io.location - FRAG_RESULT_DATA0);
   nir_def *pointer = nir_iadd(b, nir_iadd_imm(b, params,
      offsetof(struct kk_interlock_envelope, color)),
      nir_u2u64(b, nir_imul_imm(b, slot, sizeof(uint64_t))));
   nir_def *base = nir_load_global_constant(b, 1, 64, pointer,
      .align_mul = sizeof(uint64_t));
   nir_def *sample = kk_interlock_shadow_sample(b, t, params,
      intr->src[1].ssa);
   nir_def *address = nir_iadd(b, base,
      nir_u2u64(b, nir_imul_imm(b, sample, 16)));
   nir_def *words = nir_load_global(b, 4, 32, address,
      .access = nir_intrinsic_access(intr) | ACCESS_COHERENT,
      .align_mul = 16);
   nir_def *value = nir_channels(b, words, BITFIELD_RANGE(
      nir_intrinsic_component(intr), intr->def.num_components));
   if (intr->def.bit_size == 16)
      value = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) ==
         nir_type_float ? nir_f2f16(b, value) : nir_u2u16(b, value);
   return value;
}

static nir_def *
kk_interlock_quantize_depth(nir_builder *b,
   struct kk_interlock_framebuffer_nir *s, nir_def *depth)
{
   if (s->depth_bits == 16 || s->depth_bits == 24) {
      float maximum = (float)BITFIELD_MASK(s->depth_bits);
      nir_def *fixed = nir_fround_even(b,
         nir_fmul_imm(b, nir_fsat(b, depth), maximum));
      return nir_fmul_imm(b, fixed, 1.0f / maximum);
   }
   return depth;
}

static struct kk_interlock_tests_nir
kk_interlock_framebuffer_tests(nir_builder *b,
   struct kk_interlock_program_nir *program, nir_def *params,
   bool replace_reference)
{
   nir_def *front = nir_load_var(b, program->front_face);
   nir_def *stencil_offset = nir_bcsel(b, front, nir_imm_int(b, 0),
      nir_imm_int(b, sizeof(struct kk_interlock_stencil_state)));
   nir_def *stencil = nir_iadd(b, nir_iadd_imm(b, params,
      offsetof(struct kk_interlock_envelope, stencil_state)),
      nir_u2u64(b, stencil_offset));
#define BOOL_FIELD(name) nir_ine_imm(b, kk_interlock_env(b, params, name), 0)
#define STENCIL_FIELD(name) kk_interlock_env_load(b, stencil, 1, 32, \
   offsetof(struct kk_interlock_stencil_state, name))
   struct kk_interlock_tests_nir state = {
      .depth_enable = BOOL_FIELD(depth_enable),
      .depth_write = BOOL_FIELD(depth_write),
      .depth_compare = kk_interlock_env(b, params, depth_compare),
      .bounds_enable = BOOL_FIELD(bounds_enable),
      .bounds_min = kk_interlock_env(b, params, bounds_min),
      .bounds_max = kk_interlock_env(b, params, bounds_max),
      .stencil_enable = BOOL_FIELD(stencil_enable),
      .stencil = {
         .reference = replace_reference ? nir_channel(b,
            kk_interlock_load_output(b, program, FRAG_RESULT_STENCIL, 0), 0) :
            STENCIL_FIELD(reference),
         .compare_mask = STENCIL_FIELD(compare_mask),
         .write_mask = STENCIL_FIELD(write_mask),
         .compare = STENCIL_FIELD(compare),
         .fail = STENCIL_FIELD(fail),
         .depth_fail = STENCIL_FIELD(depth_fail),
         .pass = STENCIL_FIELD(pass),
      },
   };
#undef STENCIL_FIELD
#undef BOOL_FIELD
   return state;
}

/* Shadow depth and stencil are shared across all primitive batches of a tile.
 * They are read after ROG acquire; there is no snapshot-based prefilter. */
static nir_def *
kk_interlock_framebuffer_test_sample(nir_builder *b,
   struct kk_interlock_program_nir *program, nir_def *sample,
   nir_def *coverage, nir_def *depth, bool replace_reference)
{
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *index = kk_interlock_shadow_sample(b, t, params, sample);
   nir_def *depth_address = kk_interlock_shadow_address(b, params,
      offsetof(struct kk_interlock_envelope, depth), index, 4);
   nir_def *stencil_address = kk_interlock_shadow_address(b, params,
      offsetof(struct kk_interlock_envelope, stencil), index, 4);
   nir_def *old_depth = nir_load_global(b, 1, 32, depth_address, .align_mul = 4);
   nir_def *old_stencil = nir_load_global(b, 1, 32, stencil_address, .align_mul = 4);
   struct kk_interlock_tests_nir state = kk_interlock_framebuffer_tests(
      b, program, params, replace_reference);
   struct kk_interlock_test_result_nir result = kk_interlock_tests_nir(b,
      &state, coverage, kk_interlock_quantize_depth(b, s, depth),
      old_depth, old_stencil);
   nir_push_if(b, coverage);
   nir_store_global(b, result.depth, depth_address, .align_mul = 4);
   nir_store_global(b, result.stencil, stencil_address, .align_mul = 4);
   nir_pop_if(b, NULL);
   return result.passed;
}

static void
kk_interlock_count_samples(nir_builder *b, nir_def *params, nir_def *count)
{
   nir_def *address = kk_interlock_env(b, params, occlusion);
   nir_push_if(b, nir_iand(b, nir_ine_imm(b, address, 0),
                              nir_ine_imm(b, count, 0)));
   nir_def *previous = nir_global_atomic(b, 32, address, count,
                                          .atomic_op = nir_atomic_op_iadd);
   nir_push_if(b, nir_ult(b, nir_iadd(b, previous, count), previous));
   nir_global_atomic(b, 32, nir_iadd_imm(b, address, 4), nir_imm_int(b, 1),
                        .atomic_op = nir_atomic_op_iadd);
   nir_pop_if(b, NULL);
   nir_pop_if(b, NULL);
}

static void
kk_interlock_framebuffer_begin_pixel(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   kk_interlock_transaction_begin_pixel(b, program);
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *metadata = nir_load_var(b, t->metadata);
   nir_store_var(b, program->front_face,
      nir_ine_imm(b, nir_channel(b, metadata, 1), 0), 1);
   nir_def *mask = nir_load_var(b, t->coverage);
   nir_def *pipeline_mask = kk_interlock_env(b, params, sample_mask);
   nir_def *before_tests = s->post_depth_coverage ? mask :
                           nir_iand(b, mask, pipeline_mask);
   nir_store_var(b, t->coverage, before_tests, 1);
   nir_store_var(b, s->tested_coverage, before_tests, 1);
   if (!s->early)
      return;

   nir_variable *index = nir_local_variable_create(b->impl, glsl_uint_type(), NULL);
   nir_variable *passed_mask = nir_local_variable_create(b->impl, glsl_uint_type(), NULL);
   nir_store_var(b, index, nir_imm_int(b, 0), 1);
   nir_store_var(b, passed_mask, nir_imm_int(b, 0), 1);
   nir_push_loop(b);
   nir_def *sample = nir_load_var(b, index);
   nir_break_if(b, nir_uge(b, sample, kk_interlock_env(b, params, samples)));
   nir_def *bit = nir_ishl(b, nir_imm_int(b, 1), sample);
   nir_def *record = kk_interlock_transaction_record(b, t, sample);
   nir_def *zw = kk_interlock_quad_load(b,
      nir_iadd_imm(b, record, t->render->inputs.stride), 8, 2, 32,
      nir_load_var(b, t->pixel));
   nir_def *coverage = nir_ine_imm(b, nir_iand(b, before_tests, bit), 0);
   nir_def *passed = kk_interlock_framebuffer_test_sample(b, program,
      sample, coverage, nir_channel(b, zw, 0), false);
   nir_store_var(b, passed_mask, nir_ior(b, nir_load_var(b, passed_mask),
      nir_bcsel(b, passed, bit, nir_imm_int(b, 0))), 1);
   nir_store_var(b, index, nir_iadd_imm(b, sample, 1), 1);
   nir_pop_loop(b, NULL);
   nir_def *tested = nir_iand(b, nir_load_var(b, passed_mask), pipeline_mask);
   nir_store_var(b, s->tested_coverage, tested, 1);
   /* This chooses sample counting before fragment shading and multisample
    * coverage, permitted for EarlyFragmentTests by maintenance5. */
   kk_interlock_count_samples(b, params, nir_bit_count(b, tested));
}

static void
kk_interlock_framebuffer_begin_sample(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   nir_def *mask = nir_load_var(b, s->tested_coverage);
   if (!program->sample_shading) {
      /* A pixel invocation may fetch any surviving sample. Keep one source
       * invocation even when sample zero is uncovered or fails early tests. */
      nir_def *eligible = nir_bcsel(b, nir_ine_imm(b, mask, 0), mask,
         nir_load_var(b, t->coverage));
      eligible = nir_bcsel(b, nir_ine_imm(b, eligible, 0), eligible,
         nir_imm_int(b, 1));
      nir_store_var(b, program->sample, nir_find_lsb(b, eligible), 1);
   }
   kk_interlock_transaction_begin_sample(b, program);
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *sample = nir_load_var(b, program->sample);
   nir_def *helper = nir_ior(b, nir_load_var(b, program->helper),
      nir_ieq_imm(b, program->sample_shading ? nir_iand(b, mask,
         nir_ishl(b, nir_imm_int(b, 1), sample)) : mask, 0));
   nir_store_var(b, program->initial_helper, helper, 1);
   nir_store_var(b, program->helper, helper, 1);
   if (s->post_depth_coverage)
      nir_store_var(b, program->coverage, mask, 1);
   nir_def *index = kk_interlock_shadow_sample(b, t, params, sample);
   for (unsigned i = 0; i < ARRAY_SIZE(s->color_format); ++i) {
      if (s->color_format[i] == PIPE_FORMAT_NONE)
         continue;
      nir_def *address = kk_interlock_shadow_address(b, params,
         offsetof(struct kk_interlock_envelope, color) + i * sizeof(uint64_t),
         index, 16);
      kk_interlock_store_output(b, program, FRAG_RESULT_DATA0 + i, 0,
         nir_load_global(b, 4, 32, address, .align_mul = 16));
   }
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *depth = nir_channel(b, nir_load_var(b, program->coord), 2);
   kk_interlock_store_output(b, program, FRAG_RESULT_DEPTH, 0,
      nir_vec4(b, depth, zero, zero, zero));
   kk_interlock_store_output(b, program, FRAG_RESULT_SAMPLE_MASK, 0,
      nir_vec4(b, nir_imm_int(b, ~0u), zero, zero, zero));
}

/* Only the generated output function still has native output IO. Shared
 * fixed-function lowering operates on it, then redirects its framebuffer
 * reads/writes to a separate per-sample bank, never the source program bank. */
struct kk_interlock_sample_output_nir {
   struct kk_interlock_program_nir program;
   nir_function *function;
};

static bool
kk_interlock_sample_output_intrinsic(nir_builder *b,
   nir_intrinsic_instr *intr, void *data)
{
   struct kk_interlock_sample_output_nir *s = data;
   if (b->impl->function != s->function)
      return false;
   if (intr->intrinsic == nir_intrinsic_load_output ||
       intr->intrinsic == nir_intrinsic_store_output)
      return kk_interlock_output_nir(b, intr, &s->program);
   return kk_interlock_program_intrinsic(b, intr, &s->program);
}

static void
kk_interlock_framebuffer_prepare_outputs(nir_shader *shader,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_render_nir *render = s->transaction.render;
   nir_function *source = nir_shader_get_entrypoint(shader)->function;
   source->is_entrypoint = false;
   s->sample_function = nir_function_create(shader,
      "kk_interlock_sample_outputs");
   s->sample_function->is_entrypoint = true;
   nir_builder b = nir_builder_create(
      nir_function_impl_create(s->sample_function));
   b.cursor = nir_before_cf_list(&b.impl->body);
   for (unsigned i = 0; i < ARRAY_SIZE(s->color_format); ++i) {
      if (s->color_format[i] == PIPE_FORMAT_NONE ||
          !(s->outputs & BITFIELD64_BIT(FRAG_RESULT_DATA0 + i)))
         continue;
      nir_alu_type type = util_format_is_pure_sint(s->color_format[i]) ?
         nir_type_int32 : util_format_is_pure_uint(s->color_format[i]) ?
         nir_type_uint32 : nir_type_float32;
      for (unsigned dual = 0; dual < 2; ++dual) {
         if (dual && !(s->dual_outputs & BITFIELD64_BIT(FRAG_RESULT_DATA0 + i)))
            continue;
         nir_store_output(&b, kk_interlock_load_output(&b, program,
            FRAG_RESULT_DATA0 + i, dual), nir_imm_int(&b, 0),
            .src_type = type,
            .io_semantics = {.location = FRAG_RESULT_DATA0 + i,
               .num_slots = 1, .dual_source_blend_index = dual},
            .write_mask = 15);
      }
   }
   if (render->lower_sample_outputs)
      render->lower_sample_outputs(shader, render->sample_outputs_data);
   struct kk_interlock_sample_output_nir output = {
      .program = *program,
      .function = s->sample_function,
   };
   output.program.outputs = s->sample_outputs;
   nir_shader_intrinsics_pass(shader, kk_interlock_sample_output_intrinsic,
      nir_metadata_none, &output);
   s->sample_function->is_entrypoint = false;
   source->is_entrypoint = true;
}

static void
kk_interlock_framebuffer_sample_outputs(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *sample = nir_load_var(b, program->sample);
   nir_def *index = kk_interlock_shadow_sample(b, t, params, sample);
   nir_store_var(b, program->initial_helper,
      nir_load_var(b, program->helper), 1);
   struct kk_interlock_program_nir output = *program;
   output.outputs = s->sample_outputs;
   for (unsigned i = 0; i < ARRAY_SIZE(s->color_format); ++i) {
      if (s->color_format[i] == PIPE_FORMAT_NONE)
         continue;
      nir_def *address = kk_interlock_shadow_address(b, params,
         offsetof(struct kk_interlock_envelope, color) + i * sizeof(uint64_t),
         index, 16);
      kk_interlock_store_output(b, &output, FRAG_RESULT_DATA0 + i, 0,
         nir_load_global(b, 4, 32, address, .align_mul = 16));
   }
   nir_call(b, s->sample_function);
}

static void
kk_interlock_framebuffer_commit_sample(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_def *sample = nir_load_var(b, program->sample);
   nir_def *bit = nir_ishl(b, nir_imm_int(b, 1), sample);
   kk_interlock_framebuffer_sample_outputs(b, program);
   nir_def *mask = nir_channel(b,
      kk_interlock_load_output(b, program, FRAG_RESULT_SAMPLE_MASK, 0), 0);
   nir_def *coverage = nir_iand(b, kk_interlock_program_live(b, program),
      nir_ine_imm(b, nir_iand(b, mask, bit), 0));
   if (!s->early) {
      nir_def *depth;
      if (s->outputs & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
         depth = nir_channel(b,
            kk_interlock_load_output(b, program, FRAG_RESULT_DEPTH, 0), 0);
      } else {
         nir_def *record = kk_interlock_transaction_record(b, t, sample);
         depth = nir_channel(b, kk_interlock_quad_load(b,
            nir_iadd_imm(b, record, t->render->inputs.stride), 8, 2, 32,
            nir_load_var(b, t->pixel)), 0);
      }
      coverage = kk_interlock_framebuffer_test_sample(b, program, sample,
         coverage, depth, s->stencil_export);
      kk_interlock_count_samples(b, params, nir_b2i32(b, coverage));
   }
   nir_push_if(b, coverage);
   nir_def *index = kk_interlock_shadow_sample(b, t, params, sample);
   for (unsigned i = 0; i < ARRAY_SIZE(s->color_format); ++i) {
      if (s->color_format[i] == PIPE_FORMAT_NONE ||
          !(s->outputs & BITFIELD64_BIT(FRAG_RESULT_DATA0 + i)))
         continue;
      struct kk_interlock_program_nir output = *program;
      output.outputs = s->sample_outputs;
      nir_def *value = kk_interlock_load_output(b, &output,
         FRAG_RESULT_DATA0 + i, 0);
      /* Match storage precision after every primitive, including sRGB's
       * conversion and normalized/integer/half-float attachment formats. */
      value = nir_format_unpack_rgba(b,
         nir_format_pack_rgba(b, s->color_format[i], value), s->color_format[i]);
      nir_def *address = kk_interlock_shadow_address(b, params,
         offsetof(struct kk_interlock_envelope, color) + i * sizeof(uint64_t),
         index, 16);
      nir_store_global(b, value, address, .align_mul = 16);
   }
   nir_pop_if(b, NULL);
}

static void
kk_interlock_framebuffer_end_sample(nir_builder *b,
   struct kk_interlock_program_nir *program)
{
   if (program->sample_shading) {
      kk_interlock_framebuffer_commit_sample(b, program);
      return;
   }
   struct kk_interlock_framebuffer_nir *s = program->data;
   nir_variable *source_helper = nir_local_variable_create(b->impl,
      glsl_bool_type(), "pixel_source_helper");
   nir_variable *index = nir_local_variable_create(b->impl,
      glsl_uint_type(), "pixel_output_sample");
   nir_store_var(b, source_helper, nir_load_var(b, program->helper), 1);
   nir_store_var(b, index, nir_imm_int(b, 0), 1);
   nir_push_loop(b);
   nir_def *sample = nir_load_var(b, index);
   nir_break_if(b, nir_uge(b, sample, program->sample_count(b, program)));
   nir_store_var(b, program->sample, sample, 1);
   nir_def *covered = nir_ine_imm(b, nir_iand(b,
      nir_load_var(b, s->tested_coverage),
      nir_ishl(b, nir_imm_int(b, 1), sample)), 0);
   /* Alpha coverage demotion belongs to this output sample only. Restore
    * application discard/demote state before processing each next sample. */
   nir_store_var(b, program->helper, nir_ior(b,
      nir_load_var(b, source_helper), nir_inot(b, covered)), 1);
   kk_interlock_framebuffer_commit_sample(b, program);
   nir_store_var(b, index, nir_iadd_imm(b, sample, 1), 1);
   nir_pop_loop(b, NULL);
}

static void
kk_interlock_wrap_framebuffer(nir_shader *shader,
   struct kk_interlock_framebuffer_nir *s)
{
   s->outputs = shader->info.outputs_written;
   s->early = shader->info.fs.early_fragment_tests;
   s->post_depth_coverage = shader->info.fs.post_depth_coverage;
   s->stencil_export = s->outputs & BITFIELD64_BIT(FRAG_RESULT_STENCIL);
   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output &&
                nir_intrinsic_io_semantics(intr).dual_source_blend_index)
               s->dual_outputs |= BITFIELD64_BIT(
                  nir_intrinsic_io_semantics(intr).location);
         }
      }
   }
   s->tested_coverage = kk_interlock_program_var(shader,
      glsl_uint_type(), "tested_coverage");
   s->sample_outputs = kk_interlock_program_var(shader,
      glsl_array_type(glsl_uvec4_type(), 2 * FRAG_RESULT_MAX, 0),
      "sample_framebuffer_outputs");
   struct kk_interlock_program_nir program = {
      .begin_pixel = kk_interlock_framebuffer_begin_pixel,
      .begin_sample = kk_interlock_framebuffer_begin_sample,
      .end_sample = kk_interlock_framebuffer_end_sample,
      .prepare_sample_outputs = kk_interlock_framebuffer_prepare_outputs,
      .load_attachment = kk_interlock_framebuffer_load_attachment,
   };
   kk_interlock_transaction_init(shader, &s->transaction, &program);
   /* framebuffer begins with transaction; both callback families share data. */
   program.data = s;
   program.begin_pixel = kk_interlock_framebuffer_begin_pixel;
   program.begin_sample = kk_interlock_framebuffer_begin_sample;
   kk_interlock_wrap_program_nir(shader, &program);
}

#endif
