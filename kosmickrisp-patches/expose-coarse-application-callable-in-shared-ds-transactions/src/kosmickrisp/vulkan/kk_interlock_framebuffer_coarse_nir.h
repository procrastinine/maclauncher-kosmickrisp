/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_FRAMEBUFFER_COARSE_NIR_H
#define KK_INTERLOCK_FRAMEBUFFER_COARSE_NIR_H

#include "kk_interlock_framebuffer_nir.h"
#include "util/set.h"

/* One original covered sample of the caller's logical coarse invocation.
 * pixel is WINDOW-relative in the transaction envelope's indexing space;
 * coord is the ABSOLUTE captured fine FragCoord. storage_primitive addresses
 * captured records relative to primitive_base; primitive is the independent
 * shader-visible API value. No source address is inferred from PrimitiveID. */
struct kk_interlock_coarse_sample_nir {
   nir_def *pixel, *coord;
   nir_def *sample, *coverage;
   nir_def *storage_primitive, *primitive;
   nir_def *front_face, *layer, *view_index;
};

/* Host-side NIR construction only; no new packet, descriptor or scheduler.
 * The caller already owns each logical cell/quad exactly once under the
 * existing ordered primitive/job continuation and immutable capture lifetime.
 * begin_cell is quad-uniform. The callbacks preserve render->parameters and
 * its actual envelope pointer; PER_DRAW may instead hold the caller context.
 * Different original samples must occur in exactly one selected slot. */
struct kk_interlock_coarse_nir {
   nir_function *application;
   void *data;
   /* Shared virtual variables exist; the named body/typed temporaries are
    * intact. Prepare captured inputs and a fine epilogue with NO body call. */
   bool (*prepare)(nir_shader *, struct kk_interlock_program_nir *,
      nir_function *application, void *);
   nir_def *(*begin_cell)(nir_builder *, struct kk_interlock_program_nir *, void *);
   nir_def *(*sample_count)(nir_builder *, struct kk_interlock_program_nir *, void *);
   /* Before body: geometric coverage. After body: saved passing early
    * coverage, or original geometric coverage for late tests. Each coverage
    * value is the actual original pixel's full raster-sample mask. */
   struct kk_interlock_coarse_sample_nir (*sample)(nir_builder *,
      struct kk_interlock_program_nir *, nir_def *slot, bool after_body, void *);
   /* Replace the selected bit in the EXISTING coverage context. Before
    * body: passing early coverage, or geometric late coverage. After body:
    * final fine color coverage. Clearing a final bit never undoes an early
    * DS update. Keep original captured masks available for sample(). */
   void (*tested)(nir_builder *, struct kk_interlock_program_nir *,
      nir_def *slot, nir_def *passed, bool after_body, void *);
   /* Set the actual absolute coarse center, logical helper and source-sample
    * state. Demotion remains program.helper; native demote is never used to
    * suppress the later shared DS shadow writes. */
   void (*begin_body)(nir_builder *, struct kk_interlock_program_nir *,
      nir_def *any_surviving, void *);
   void (*end_body)(nir_builder *, struct kk_interlock_program_nir *, void *);
   /* Project the retained typed packet/fine epilogue into program.outputs
    * and helper state for this original sample. Never call the body or test
    * or mutate DS here. Callables must be prepared before shared virtual
    * lowering; fresh native discard/helper operations are invalid. The
    * shared transaction is the sole commit owner. */
   void (*outputs)(nir_builder *, struct kk_interlock_program_nir *,
      nir_def *slot, void *);
};

static bool
kk_interlock_coarse_bind_sample(nir_builder *b,
   struct kk_interlock_program_nir *program,
   const struct kk_interlock_coarse_sample_nir *sample)
{
   if (!sample->pixel || !sample->coord || !sample->sample || !sample->coverage ||
       !sample->storage_primitive || !sample->primitive || !sample->front_face ||
       !sample->layer || !sample->view_index)
      return false;
   struct kk_interlock_framebuffer_nir *s = program->data;
   struct kk_interlock_transaction_nir *t = &s->transaction;
   nir_def *params = t->render->parameters(b, t->render->data);
   nir_store_var(b, t->pixel, sample->pixel, 3);
   nir_store_var(b, t->primitive, sample->storage_primitive, 1);
   nir_store_var(b, t->anchor, kk_interlock_quad_anchor(b, params,
      sample->pixel, sample->storage_primitive), 1);
   nir_store_var(b, t->coverage, sample->coverage, 1);
   nir_store_var(b, program->sample, sample->sample, 1);
   nir_store_var(b, program->coord, sample->coord, 15);
   nir_store_var(b, program->coverage, sample->coverage, 1);
   nir_store_var(b, program->primitive, sample->primitive, 1);
   nir_store_var(b, program->front_face, sample->front_face, 1);
   nir_store_var(b, program->layer, sample->layer, 1);
   nir_store_var(b, program->view_index, sample->view_index, 1);
   nir_store_var(b, program->sample_position,
      kk_interlock_transaction_sample_position(b, program, sample->sample), 3);
   return true;
}

/* A fine callable may not re-enter the application, even indirectly. Count
 * reachable call sites before inlining destroys that useful ownership seam. */
static unsigned
kk_interlock_coarse_body_calls(nir_function *function, nir_function *application,
   struct set *visited)
{
   if (!function->impl || _mesa_set_search(visited, function))
      return 0;
   _mesa_set_add(visited, function);
   unsigned calls = 0;
   nir_foreach_block(block, function->impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_call)
            continue;
         nir_function *callee = nir_instr_as_call(instr)->callee;
         calls += callee == application ? 1 :
            kk_interlock_coarse_body_calls(callee, application, visited);
         if (calls > 1)
            return calls;
      }
   }
   return calls;
}

/* Compilation clone of the retained linked module. Neither application
 * splitting, preprocessing, descriptor finishing nor per-vertex lowering is
 * repeated. Ordinary framebuffer callers do not enter this constructor. */
static bool
kk_interlock_wrap_framebuffer_coarse(nir_shader *shader,
   struct kk_interlock_framebuffer_nir *s,
   const struct kk_interlock_coarse_nir *coarse)
{
   if (!shader || shader->info.stage != MESA_SHADER_FRAGMENT || !coarse ||
       !coarse->application || !coarse->application->impl ||
       coarse->application->num_params || !coarse->prepare || !coarse->begin_cell ||
       !coarse->sample_count || !coarse->sample || !coarse->tested ||
       !coarse->begin_body || !coarse->end_body || !coarse->outputs ||
       !s || !s->transaction.render || !s->transaction.render->parameters)
      return false;
   bool owns_body = false;
   nir_foreach_function(function, shader)
      owns_body |= function == coarse->application;
   if (!owns_body)
      return false;
   kk_interlock_framebuffer_init_nir(shader, s);
   struct kk_interlock_program_nir program = {
      .load_attachment = kk_interlock_framebuffer_load_attachment,
   };
   kk_interlock_transaction_init(shader, &s->transaction, &program);
   program.data = s;
   /* VRS owns logical captured-input/coverage lowering. Shared virtual
    * intrinsics and helper-side-effect masking still use the same program. */
   program.lower_inputs = NULL;
   kk_interlock_program_init_nir(shader, &program);
   s->sample_outputs = program.outputs;
   if (!coarse->prepare(shader, &program, coarse->application, coarse->data))
      return false;
   kk_interlock_program_lower_nir(shader, &program, coarse->application->impl);

   nir_shader_get_entrypoint(shader)->function->is_entrypoint = false;
   coarse->application->is_entrypoint = false;
   nir_function *entry = nir_function_create(shader, "kk_interlock_coarse_transaction");
   entry->is_entrypoint = true;
   nir_builder b = nir_builder_create(nir_function_impl_create(entry));
   b.cursor = nir_before_cf_list(&b.impl->body);
   nir_def *active = coarse->begin_cell(&b, &program, coarse->data);
   if (!active)
      return false;
   nir_push_if(&b, active);
   nir_begin_invocation_interlock(&b);
   nir_scoped_memory_barrier(&b, SCOPE_DEVICE, NIR_MEMORY_ACQUIRE,
      nir_var_mem_global | nir_var_image | nir_var_mem_ssbo);
   nir_def *params = s->transaction.render->parameters(&b, s->transaction.render->data);
   nir_def *samples = coarse->sample_count(&b, &program, coarse->data);
   if (!samples)
      return false;
   nir_def *pipeline_mask = kk_interlock_env(&b, params, sample_mask);
   nir_variable *slot = nir_local_variable_create(b.impl, glsl_uint_type(), "coarse_original_sample");
   nir_variable *any = nir_local_variable_create(b.impl, glsl_bool_type(), "coarse_surviving");
   nir_variable *count = nir_local_variable_create(b.impl, glsl_uint_type(), "coarse_passed_samples");
   nir_store_var(&b, slot, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, any, nir_imm_false(&b), 1);
   nir_store_var(&b, count, nir_imm_int(&b, 0), 1);
   nir_push_loop(&b);
   nir_def *index = nir_load_var(&b, slot);
   nir_break_if(&b, nir_uge(&b, index, samples));
   struct kk_interlock_coarse_sample_nir sample =
      coarse->sample(&b, &program, index, false, coarse->data);
   if (!kk_interlock_coarse_bind_sample(&b, &program, &sample))
      return false;
   nir_def *bit = nir_ishl(&b, nir_imm_int(&b, 1), sample.sample);
   nir_def *pipeline_covered = nir_ine_imm(&b, nir_iand(&b, pipeline_mask, bit), 0);
   nir_def *covered = nir_ine_imm(&b, nir_iand(&b, sample.coverage, bit), 0);
   nir_def *passed = covered;
   if (s->early) {
      nir_def *test_coverage = s->post_depth_coverage ? covered :
         nir_iand(&b, covered, pipeline_covered);
      passed = kk_interlock_framebuffer_test_sample(&b, &program, sample.sample,
         test_coverage, nir_channel(&b, sample.coord, 2), false);
   }
   passed = nir_iand(&b, passed, pipeline_covered);
   coarse->tested(&b, &program, index, passed, false, coarse->data);
   nir_store_var(&b, any, nir_ior(&b, nir_load_var(&b, any), passed), 1);
   if (s->early)
      nir_store_var(&b, count, nir_iadd(&b, nir_load_var(&b, count), nir_b2i32(&b, passed)), 1);
   nir_store_var(&b, slot, nir_iadd_imm(&b, index, 1), 1);
   nir_pop_loop(&b, NULL);
   if (s->early)
      kk_interlock_count_samples(&b, params, nir_load_var(&b, count));

   nir_def *surviving = nir_load_var(&b, any);
   nir_store_var(&b, program.initial_helper, nir_inot(&b, surviving), 1);
   nir_store_var(&b, program.helper, nir_inot(&b, surviving), 1);
   nir_push_if(&b, nir_quad_vote_any(&b, 1, surviving));
   coarse->begin_body(&b, &program, surviving, coarse->data);
   /* Exactly one source invocation per caller-owned logical cell/quad. */
   nir_call(&b, coarse->application);
   coarse->end_body(&b, &program, coarse->data);
   nir_pop_if(&b, NULL);
   nir_variable *body_helper = nir_local_variable_create(b.impl,
      glsl_bool_type(), "coarse_body_helper");
   nir_store_var(&b, body_helper, nir_load_var(&b, program.helper), 1);
   nir_store_var(&b, slot, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, count, nir_imm_int(&b, 0), 1);
   nir_push_loop(&b);
   index = nir_load_var(&b, slot);
   nir_break_if(&b, nir_uge(&b, index, samples));
   sample = coarse->sample(&b, &program, index, true, coarse->data);
   if (!kk_interlock_coarse_bind_sample(&b, &program, &sample))
      return false;
   bit = nir_ishl(&b, nir_imm_int(&b, 1), sample.sample);
   covered = nir_ine_imm(&b, nir_iand(&b, nir_iand(&b,
      sample.coverage, pipeline_mask), bit), 0);
   nir_def *helper = nir_ior(&b, nir_load_var(&b, body_helper), nir_inot(&b, covered));
   nir_store_var(&b, program.initial_helper, helper, 1);
   nir_store_var(&b, program.helper, helper, 1);
   coarse->outputs(&b, &program, index, coarse->data);
   passed = kk_interlock_framebuffer_commit_sample_impl(&b, &program, false, false);
   coarse->tested(&b, &program, index, passed, true, coarse->data);
   if (!s->early)
      nir_store_var(&b, count, nir_iadd(&b, nir_load_var(&b, count), nir_b2i32(&b, passed)), 1);
   nir_store_var(&b, slot, nir_iadd_imm(&b, index, 1), 1);
   nir_pop_loop(&b, NULL);
   if (!s->early)
      kk_interlock_count_samples(&b, params, nir_load_var(&b, count));
   nir_scoped_memory_barrier(&b, SCOPE_DEVICE, NIR_MEMORY_RELEASE,
      nir_var_mem_global | nir_var_image | nir_var_mem_ssbo);
   nir_end_invocation_interlock(&b);
   nir_pop_if(&b, NULL);
   struct set *visited = _mesa_pointer_set_create(NULL);
   if (!visited)
      return false;
   unsigned calls = kk_interlock_coarse_body_calls(entry,
      coarse->application, visited);
   _mesa_set_destroy(visited, NULL);
   if (calls != 1)
      return false;
   kk_interlock_program_finish_nir(shader);
   /* Callbacks added after virtual lowering must not demote the native
    * invocation that still owns the final fine shadow commits. */
   nir_foreach_block(block, nir_shader_get_entrypoint(shader)) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         switch (nir_instr_as_intrinsic(instr)->intrinsic) {
         case nir_intrinsic_demote:
         case nir_intrinsic_demote_if:
         case nir_intrinsic_terminate:
         case nir_intrinsic_terminate_if:
         case nir_intrinsic_load_helper_invocation:
         case nir_intrinsic_is_helper_invocation:
            return false;
         default:
            break;
         }
      }
   }
   return true;
}
#endif
