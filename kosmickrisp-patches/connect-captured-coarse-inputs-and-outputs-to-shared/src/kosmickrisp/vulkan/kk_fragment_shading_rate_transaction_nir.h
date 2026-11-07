/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_TRANSACTION_NIR_H
#define KK_FRAGMENT_SHADING_RATE_TRANSACTION_NIR_H

#include "kosmickrisp/compiler/kk_fragment_shading_rate.h"
#include "kk_fragment_shading_rate_inputs_nir.h"
#include "kk_interlock_framebuffer_coarse_nir.h"

/* Construction-time view of the caller's existing ordered primitive job.
 * cells supplies begin_cell/sample_count/sample/tested/begin_body and data.
 * Its original masks and unique quad ownership stay with that scheduler.
 * inputs describes its immutable captured anchor, separate from the coarse
 * center written to program.coord by begin_body. That callback also fills
 * logical lane/rate/original_per_draw and the selected original sample.
 * sample_shift is r*((fw*fh)-1-p), not the original sample number. */
struct kk_vrs_transaction_nir {
   struct kk_interlock_coarse_nir cells;
   struct kk_vrs_logical_variables logical;
   struct kk_vrs_captured_inputs_nir inputs;
   nir_def *(*sample_shift)(nir_builder *, struct kk_interlock_program_nir *,
      nir_def *slot, void *);
   unsigned samples;
};

struct kk_vrs_transaction_build {
   const struct kk_vrs_transaction_nir *source;
   struct kk_interlock_framebuffer_nir *framebuffer;
   nir_function *fine;
   struct kk_vrs_local_outputs outputs;
   nir_variable *shift;
};

static bool
kk_vrs_transaction_prepare(nir_shader *shader,
   struct kk_interlock_program_nir *program, nir_function *application, void *data)
{
   struct kk_vrs_transaction_build *s = data;
   const struct kk_vrs_transaction_nir *source = s->source;
   struct kk_interlock_transaction_nir *transaction = &s->framebuffer->transaction;
   struct kk_vrs_captured_inputs_nir inputs = source->inputs;
   inputs.capture.capture = &transaction->render->inputs;
   inputs.capture.sample = program->sample;
   inputs.center = program->coord;
   struct kk_vrs_logical_variables logical = source->logical;
   logical.initial_helper = program->initial_helper;
   if (!kk_vrs_lower_captured_inputs(shader, application, &inputs) ||
       !kk_nir_vrs_lower_transaction_body(shader, application, &logical) ||
       !kk_nir_vrs_rebase_fine_draw(s->fine, logical.original_per_draw))
      return false;

   /* Fine fixed-function inputs read the current original sample/anchor,
    * while application interpolation was lowered at its logical center. */
   struct kk_interlock_captured_inputs_nir fine = {
      .capture = &transaction->render->inputs,
      .sample_shading = true,
      .sample = program->sample,
      .coord = program->coord,
      .data = transaction,
      .record = kk_interlock_transaction_record,
      .sample_position = kk_interlock_captured_sample_position,
   };
   nir_function_intrinsics_pass(s->fine->impl, kk_interlock_captured_input,
      nir_metadata_none, &fine);
   s->shift = kk_interlock_program_var(shader, glsl_uint_type(), "fine_mask_shift");
   if (!kk_nir_vrs_local_outputs(shader, s->shift, source->samples, &s->outputs))
      return false;
   s->framebuffer->sample_function = s->fine;
   return true;
}

static nir_def *
kk_vrs_transaction_begin_cell(nir_builder *b,
   struct kk_interlock_program_nir *program, void *data)
{
   const struct kk_interlock_coarse_nir *c =
      &((struct kk_vrs_transaction_build *)data)->source->cells;
   return c->begin_cell(b, program, c->data);
}

static nir_def *
kk_vrs_transaction_sample_count(nir_builder *b,
   struct kk_interlock_program_nir *program, void *data)
{
   const struct kk_interlock_coarse_nir *c =
      &((struct kk_vrs_transaction_build *)data)->source->cells;
   return c->sample_count(b, program, c->data);
}

static struct kk_interlock_coarse_sample_nir
kk_vrs_transaction_sample(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *slot, bool after_body, void *data)
{
   const struct kk_interlock_coarse_nir *c =
      &((struct kk_vrs_transaction_build *)data)->source->cells;
   return c->sample(b, program, slot, after_body, c->data);
}

static void
kk_vrs_transaction_tested(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *slot, nir_def *passed, bool after_body, void *data)
{
   const struct kk_interlock_coarse_nir *c =
      &((struct kk_vrs_transaction_build *)data)->source->cells;
   c->tested(b, program, slot, passed, after_body, c->data);
}

static void
kk_vrs_transaction_begin_body(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *surviving, void *data)
{
   const struct kk_interlock_coarse_nir *c =
      &((struct kk_vrs_transaction_build *)data)->source->cells;
   c->begin_body(b, program, surviving, c->data);
}

static void
kk_vrs_transaction_end_body(nir_builder *b,
   struct kk_interlock_program_nir *program, void *data)
{
   struct kk_vrs_transaction_build *s = data;
   /* All backups remain invocation-local, including helper lanes. No helper
    * global write or second result/scratch packet is introduced. */
   nir_call(b, s->outputs.save);
}

static void
kk_vrs_transaction_outputs(nir_builder *b, struct kk_interlock_program_nir *program,
   nir_def *slot, void *data)
{
   struct kk_vrs_transaction_build *s = data;
   nir_def *shift = s->source->sample_shift(b, program, slot, s->source->cells.data);
   assert(shift && shift->num_components == 1 && shift->bit_size == 32);
   nir_store_var(b, s->shift, shift, 1);
   nir_call(b, s->outputs.restore);
   /* Supply the existing shared bank defaults for outputs the source did
    * not export. The real fine function may replace them for this sample. */
   nir_def *zero = nir_imm_int(b, 0);
   kk_interlock_store_output(b, program, FRAG_RESULT_DEPTH, 0,
      nir_vec4(b, nir_channel(b, nir_load_var(b, program->coord), 2), zero, zero, zero));
   kk_interlock_store_output(b, program, FRAG_RESULT_SAMPLE_MASK, 0,
      nir_vec4(b, nir_imm_int(b, ~0u), zero, zero, zero));
   /* Loads actual current color shadows, then calls only the retained fine
    * epilogue. Shared commit_sample remains the sole color/DS writer. */
   kk_interlock_framebuffer_sample_outputs(b, program);
}

/* Retained linked compilation clone only. Capture, pre-raster and descriptor
 * finishing have already happened. The borrowed construction objects expire
 * on return; only their generated NIR remains. Native DS must be BYPASS and
 * the caller must use the same ordered software transaction for replay. */
static bool
kk_vrs_wrap_framebuffer_transaction(nir_shader *shader,
   struct kk_interlock_framebuffer_nir *framebuffer,
   const struct kk_vrs_transaction_nir *source, unsigned *local_output_bytes)
{
   if (!shader || !framebuffer || !source || !source->sample_shift ||
       !source->cells.begin_cell || !source->cells.sample_count ||
       !source->cells.sample || !source->cells.tested || !source->cells.begin_body ||
       (source->samples != 1 && source->samples != 2 && source->samples != 4))
      return false;
   struct kk_vrs_transaction_build state = {
      .source = source, .framebuffer = framebuffer,
   };
   nir_function *application;
   if (!kk_nir_vrs_take_fine_epilogue(shader, &application, &state.fine))
      return false;
   struct kk_interlock_coarse_nir coarse = {
      .application = application, .data = &state,
      .prepare = kk_vrs_transaction_prepare,
      .begin_cell = kk_vrs_transaction_begin_cell,
      .sample_count = kk_vrs_transaction_sample_count,
      .sample = kk_vrs_transaction_sample,
      .tested = kk_vrs_transaction_tested,
      .begin_body = kk_vrs_transaction_begin_body,
      .end_body = kk_vrs_transaction_end_body,
      .outputs = kk_vrs_transaction_outputs,
   };
   bool result = kk_interlock_wrap_framebuffer_coarse(shader, framebuffer, &coarse);
   if (result && local_output_bytes)
      *local_output_bytes = state.outputs.bytes;
   return result;
}
#endif
