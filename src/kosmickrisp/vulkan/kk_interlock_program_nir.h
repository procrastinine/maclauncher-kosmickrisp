/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_PROGRAM_NIR_H
#define KK_INTERLOCK_PROGRAM_NIR_H

#include "compiler/nir/nir_builder.h"

/* One physical fragment owns the pixel transaction. The original program is a
 * temporary NIR function, called at the source's pixel/sample frequency and
 * inlined before the ordinary compiler sees it. Shader temporaries bridge it
 * and the per-sample framebuffer function;
 * no NIR, shader variants or alternate MSL emitter survive compilation. */
struct kk_interlock_program_nir {
   bool sample_shading;
   nir_variable *coord;
   nir_variable *sample;
   nir_variable *sample_position;
   nir_variable *coverage;
   nir_variable *primitive;
   nir_variable *front_face;
   nir_variable *layer;
   nir_variable *view_index;
   nir_variable *point_coord;
   nir_variable *initial_helper;
   nir_variable *helper;
   nir_variable *outputs;
   void *data;
   void (*lower_inputs)(nir_shader *, struct kk_interlock_program_nir *);
   void (*prepare_sample_outputs)(nir_shader *, struct kk_interlock_program_nir *);
   nir_def *(*load_attachment)(nir_builder *, struct kk_interlock_program_nir *,
      nir_intrinsic_instr *);
   nir_def *(*sample_count)(nir_builder *, struct kk_interlock_program_nir *);
   nir_def *(*sample_position_for_id)(nir_builder *,
      struct kk_interlock_program_nir *, nir_def *);
   void (*begin_pixel)(nir_builder *, struct kk_interlock_program_nir *);
   nir_def *(*active_quad)(nir_builder *, struct kk_interlock_program_nir *);
   void (*begin_sample)(nir_builder *, struct kk_interlock_program_nir *);
   void (*end_sample)(nir_builder *, struct kk_interlock_program_nir *);
   void (*end_pixel)(nir_builder *, struct kk_interlock_program_nir *);
};

static nir_variable *
kk_interlock_program_var(nir_shader *shader, const struct glsl_type *type,
                          const char *name)
{
   return nir_variable_create(shader, nir_var_shader_temp, type, name);
}

static nir_def *
kk_interlock_program_live(nir_builder *b, struct kk_interlock_program_nir *s)
{
   return nir_inot(b, nir_load_var(b, s->helper));
}

/* Raw 32-bit output words preserve typed integer colors as well as floats.
 * The transaction initializes framebuffer-fetch colors before each call and
 * consumes depth, stencil and coverage only after that virtual call returns. */
static nir_deref_instr *
kk_interlock_output_deref(nir_builder *b, struct kk_interlock_program_nir *s,
                           unsigned location, unsigned dual, nir_def *offset)
{
   return nir_build_deref_array(b, nir_build_deref_var(b, s->outputs),
      nir_iadd_imm(b, offset, location + dual * FRAG_RESULT_MAX));
}

static nir_def *
kk_interlock_load_output(nir_builder *b, struct kk_interlock_program_nir *s,
                          unsigned location, unsigned dual)
{
   return nir_load_deref(b, kk_interlock_output_deref(b, s, location, dual,
      nir_imm_int(b, 0)));
}

static void
kk_interlock_store_output(nir_builder *b, struct kk_interlock_program_nir *s,
                           unsigned location, unsigned dual, nir_def *words)
{
   nir_store_deref(b, kk_interlock_output_deref(b, s, location, dual,
      nir_imm_int(b, 0)), words, 15);
}

static bool
kk_interlock_output_nir(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct kk_interlock_program_nir *s = data;
   bool store = intr->intrinsic == nir_intrinsic_store_output;
   if (!store && intr->intrinsic != nir_intrinsic_load_output)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   nir_deref_instr *dst = kk_interlock_output_deref(b, s, io.location,
      io.dual_source_blend_index, intr->src[store ? 1 : 0].ssa);
   nir_def *words = nir_load_deref(b, dst);
   unsigned component = nir_intrinsic_component(intr);
   if (store) {
      nir_def *value = intr->src[0].ssa;
      nir_alu_type type = nir_intrinsic_src_type(intr);
      if (value->bit_size == 16) {
         nir_alu_type base = nir_alu_type_get_base_type(type);
         value = base == nir_type_float ? nir_f2f32(b, value) :
                 base == nir_type_int ? nir_i2i32(b, value) : nir_u2u32(b, value);
      }
      assert(value->bit_size == 32);
      for (unsigned i = 0; i < intr->num_components; ++i)
         if (nir_intrinsic_write_mask(intr) & BITFIELD_BIT(i))
            words = nir_vector_insert_imm(b, words,
               nir_channel(b, value, i), component + i);
      nir_store_deref(b, dst, words, 15);
      nir_instr_remove(&intr->instr);
   } else {
      nir_def *value = nir_channels(b, words,
         BITFIELD_RANGE(component, intr->def.num_components));
      if (intr->def.bit_size == 16)
         value = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) == nir_type_float
            ? nir_f2f16(b, value) : nir_u2u16(b, value);
      nir_def_replace(&intr->def, value);
   }
   return true;
}

static bool
kk_interlock_program_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                                void *data)
{
   struct kk_interlock_program_nir *s = data;
   nir_def *value = NULL;
   nir_variable *var = NULL;
   b->cursor = nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_tile_image:
      if (!s->load_attachment)
         return false;
      value = s->load_attachment(b, s, intr);
      if (!value)
         return false;
      break;
   case nir_intrinsic_load_frag_coord: var = s->coord; break;
   case nir_intrinsic_load_sample_id: var = s->sample; break;
   case nir_intrinsic_load_sample_pos: var = s->sample_position; break;
   case nir_intrinsic_load_sample_pos_from_id:
      if (!s->sample_position_for_id)
         return false;
      value = s->sample_position_for_id(b, s, intr->src[0].ssa);
      break;
   case nir_intrinsic_load_sample_mask_in: var = s->coverage; break;
   case nir_intrinsic_load_primitive_id: var = s->primitive; break;
   case nir_intrinsic_load_front_face: var = s->front_face; break;
   case nir_intrinsic_load_layer_id: var = s->layer; break;
   case nir_intrinsic_load_view_index: var = s->view_index; break;
   case nir_intrinsic_load_point_coord: var = s->point_coord; break;
   case nir_intrinsic_load_helper_invocation: var = s->initial_helper; break;
   case nir_intrinsic_is_helper_invocation: var = s->helper; break;
   case nir_intrinsic_increment_pipeline_statistic_kk:
      /* Virtual helpers are ordinary physical lanes. Reuse their existing
       * live predicate; driver counters must not add a native helper input. */
      nir_push_if(b, kk_interlock_program_live(b, s));
      nir_increment_pipeline_statistic_kk(b, intr->src[0].ssa);
      nir_pop_if(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
      value = nir_load_var(b, s->helper);
      value = nir_ior(b, value, intr->intrinsic == nir_intrinsic_demote
         ? nir_imm_true(b) : intr->src[0].ssa);
      nir_store_var(b, s->helper, value, 1);
      nir_instr_remove(&intr->instr);
      return true;
   case nir_intrinsic_begin_invocation_interlock:
   case nir_intrinsic_end_invocation_interlock:
      /* SPIR-V memory fences remain in the program. The wrapper owns the
       * native ROG access and serializes every region and virtual sample. */
      nir_instr_remove(&intr->instr);
      return true;
   case nir_intrinsic_ballot:
      nir_src_rewrite(&intr->src[0], nir_iand(b, intr->src[0].ssa,
         kk_interlock_program_live(b, s)));
      return true;
   case nir_intrinsic_vote_any:
      nir_src_rewrite(&intr->src[0], nir_iand(b, intr->src[0].ssa,
         kk_interlock_program_live(b, s)));
      return true;
   case nir_intrinsic_vote_all:
      nir_src_rewrite(&intr->src[0], nir_ior(b, intr->src[0].ssa,
         nir_load_var(b, s->helper)));
      return true;
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan: {
      nir_const_value identity = nir_alu_binop_identity(
         nir_intrinsic_reduction_op(intr), intr->src[0].ssa->bit_size);
      nir_def *neutral = nir_build_imm(b, 1,
         intr->src[0].ssa->bit_size, &identity);
      nir_src_rewrite(&intr->src[0], nir_bcsel(b,
         kk_interlock_program_live(b, s), intr->src[0].ssa, neutral));
      return true;
   }
   case nir_intrinsic_elect:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq: {
      nir_def *live = kk_interlock_program_live(b, s);
      /* Match the backend's native 32-lane subgroup ballot. */
      nir_def *mask = nir_ballot(b, 1, 32, live);
      nir_def *first = nir_find_lsb(b, mask);
      if (intr->intrinsic == nir_intrinsic_elect) {
         value = nir_iand(b, live,
            nir_ieq(b, nir_load_subgroup_invocation(b), first));
      } else {
         nir_def *leader = nir_read_invocation(b, intr->src[0].ssa, first);
         if (intr->intrinsic == nir_intrinsic_read_first_invocation) {
            value = leader;
         } else {
            nir_def *equal = intr->intrinsic == nir_intrinsic_vote_feq
               ? nir_feq(b, intr->src[0].ssa, leader)
               : nir_ieq(b, intr->src[0].ssa, leader);
            value = nir_vote_all(b, 1,
               nir_ior(b, equal, nir_inot(b, live)));
         }
      }
      break;
   }
   default:
      return false;
   }
   if (var)
      value = nir_load_var(b, var);
   nir_def_replace(&intr->def, value);
   return true;
}

static void
kk_interlock_wrap_program_nir(nir_shader *shader,
                               struct kk_interlock_program_nir *s)
{
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);
   NIR_PASS(_, shader, nir_inline_functions);
   nir_remove_non_entrypoints(shader);
   nir_function_impl *program = nir_shader_get_entrypoint(shader);
   assert(program->function->num_params == 0);
   s->coord = kk_interlock_program_var(shader, glsl_vec4_type(), "virtual_coord");
   s->sample = kk_interlock_program_var(shader, glsl_uint_type(), "virtual_sample");
   s->sample_position = kk_interlock_program_var(shader,
      glsl_vec2_type(), "virtual_sample_position");
   s->coverage = kk_interlock_program_var(shader, glsl_uint_type(), "virtual_coverage");
   s->primitive = kk_interlock_program_var(shader, glsl_uint_type(), "virtual_primitive");
   s->front_face = kk_interlock_program_var(shader, glsl_bool_type(), "virtual_front_face");
   s->layer = kk_interlock_program_var(shader, glsl_uint_type(), "virtual_layer");
   s->view_index = kk_interlock_program_var(shader, glsl_uint_type(), "virtual_view_index");
   s->point_coord = kk_interlock_program_var(shader, glsl_vec2_type(), "virtual_point_coord");
   s->initial_helper = kk_interlock_program_var(shader, glsl_bool_type(), "virtual_initial_helper");
   s->helper = kk_interlock_program_var(shader, glsl_bool_type(), "virtual_helper");

   s->outputs = kk_interlock_program_var(shader,
      glsl_array_type(glsl_uvec4_type(), 2 * FRAG_RESULT_MAX, 0), "virtual_outputs");
   nir_shader_intrinsics_pass(shader, kk_interlock_output_nir,
                               nir_metadata_none, s);
   if (s->lower_inputs)
      s->lower_inputs(shader, s);
   NIR_PASS(_, shader, nir_lower_terminate_to_demote);
   /* A source terminate exits only this virtual invocation. Inlining turns
    * these function returns into structured control flow within the loop. */
   nir_foreach_block(block, program) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_jump &&
             nir_instr_as_jump(instr)->type == nir_jump_halt)
            nir_instr_as_jump(instr)->type = nir_jump_return;
      }
   }
   NIR_PASS(_, shader, nir_lower_helper_writes, true);
   nir_shader_intrinsics_pass(shader, kk_interlock_program_intrinsic,
                               nir_metadata_none, s);
   if (s->prepare_sample_outputs)
      s->prepare_sample_outputs(shader, s);

   program->function->is_entrypoint = false;
   nir_function *entry = nir_function_create(shader, "kk_interlock_transaction");
   entry->is_entrypoint = true;
   nir_builder b = nir_builder_create(nir_function_impl_create(entry));
   b.cursor = nir_before_cf_list(&b.impl->body);
   nir_def *samples = s->sample_shading ? s->sample_count(&b, s) :
                                        nir_imm_int(&b, 1);
   nir_variable *index = nir_local_variable_create(b.impl,
      glsl_uint_type(), "sample_index");
   nir_store_var(&b, index, nir_imm_int(&b, 0), 1);
   nir_begin_invocation_interlock(&b);
   nir_scoped_memory_barrier(&b, SCOPE_DEVICE,
      NIR_MEMORY_ACQUIRE, nir_var_mem_global | nir_var_image | nir_var_mem_ssbo);
   if (s->begin_pixel)
      s->begin_pixel(&b, s);
   if (s->active_quad)
      nir_push_if(&b, s->active_quad(&b, s));
   nir_push_loop(&b);
   nir_def *sample = nir_load_var(&b, index);
   nir_break_if(&b, nir_uge(&b, sample, samples));
   nir_store_var(&b, s->sample, sample, 1);
   s->begin_sample(&b, s);
   nir_push_if(&b, nir_quad_vote_any(&b, 1, kk_interlock_program_live(&b, s)));
   nir_call(&b, program->function);
   nir_pop_if(&b, NULL);
   s->end_sample(&b, s);
   nir_store_var(&b, index, nir_iadd_imm(&b, sample, 1), 1);
   nir_pop_loop(&b, NULL);
   if (s->end_pixel)
      s->end_pixel(&b, s);
   if (s->active_quad)
      nir_pop_if(&b, NULL);
   nir_scoped_memory_barrier(&b, SCOPE_DEVICE,
      NIR_MEMORY_RELEASE, nir_var_mem_global | nir_var_image | nir_var_mem_ssbo);
   nir_end_invocation_interlock(&b);

   shader->info.fs.uses_sample_shading = false;
   shader->info.fs.early_fragment_tests = false;
   shader->info.fs.post_depth_coverage = false;
   NIR_PASS(_, shader, nir_lower_returns);
   NIR_PASS(_, shader, nir_inline_functions);
   nir_remove_non_entrypoints(shader);
   NIR_PASS(_, shader, nir_lower_global_vars_to_local);
   NIR_PASS(_, shader, nir_lower_vars_to_ssa);
   nir_shader_gather_info(shader, nir_shader_get_entrypoint(shader));
}

#endif
