/*
 * Copyright © 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "kosmickrisp-rt-cps.h"
#include "nir/nir_builder.h"
#include "util/u_math.h"
#include <string.h>

bool
kk_rt_prepare_context(nir_shader *shader)
{
   nir_function *function=nir_shader_get_entrypoint(shader)->function;
   if(function->num_params){
      assert(function->num_params==1&&function->params[0].num_components==1&&function->params[0].bit_size==64);
      return true;
   }
   nir_parameter *param=ralloc_array(function,nir_parameter,1);
   if(!param)return false;
   *param=(nir_parameter){.num_components=1,.bit_size=64};
   function->params=param;function->num_params=1;return true;
}

static nir_def *
context(nir_builder *b)
{
   return nir_load_param(b, 0);
}

static nir_def *
context_load(nir_builder *b, unsigned offset, unsigned count, unsigned bits)
{
   return nir_load_global(b, count, bits, nir_iadd_imm(b, context(b), offset),
                          .align_mul = bits / 8);
}

static void
context_store(nir_builder *b, unsigned offset, nir_def *value)
{
   nir_store_global(b, value, nir_iadd_imm(b, context(b), offset),
                     .align_mul = value->bit_size / 8);
}

#define load_field(b, field, count, bits) \
   context_load(b, offsetof(struct kk_rt_cps_context, field), count, bits)
#define store_field(b, field, value) \
   context_store(b, offsetof(struct kk_rt_cps_context, field), value)

nir_def *
kk_rt_load_embedded_sampler_table(nir_builder *b)
{
   return load_field(b,embedded_sampler_table,1,64);
}

/* Mesa's explicit-IO lowering requires every deref to have the bit size of
 * its address format. SPIR-V gives these derefs 32 bits, but this splitter
 * lowers them to 64-bit global addresses. The lowering walks derefs in
 * reverse, so a 32-bit child would add its offset to its parent at 32 bits
 * before the parent becomes a 64-bit address. A cast of a non-deref value
 * already carries that value's size. */
static void
widen_derefs(nir_shader *shader, nir_variable_mode modes)
{
   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (nir_deref_mode_is_in_set(deref, modes) &&
                (deref->deref_type != nir_deref_type_cast ||
                 nir_src_is_deref(deref->parent)))
               deref->def.bit_size = 64;
         }
      }
   }
}

/* Incoming payload and hit-attribute derefs follow Mesa's explicit-IO route
 * (lvp_ray_tracing_pipeline.c, Copyright2024 Valve Corporation, MIT), using
 * full-width global addresses supplied by the active continuation context. */
static bool
lower_call_data(nir_shader *shader, uint64_t *payload_size, uint64_t *attribute_size)
{
   bool progress = false;
   NIR_PASS(_, shader, nir_lower_vars_to_explicit_types,
      nir_var_shader_call_data | nir_var_ray_hit_attrib, glsl_get_natural_size_align_bytes);
   nir_foreach_variable_with_modes(var, shader, nir_var_shader_call_data | nir_var_ray_hit_attrib) {
      unsigned size, alignment;
      glsl_get_natural_size_align_bytes(var->type, &size, &alignment);
      uint64_t end = (uint64_t)var->data.driver_location + size;
      if (var->data.mode == nir_var_shader_call_data)
         *payload_size = MAX2(*payload_size, end);
      else
         *attribute_size = MAX2(*attribute_size, end);
   }
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_deref)
            continue;
         nir_deref_instr *deref = nir_instr_as_deref(instr);
         if (!nir_deref_mode_is_one_of(deref, nir_var_shader_call_data | nir_var_ray_hit_attrib))
            continue;
         bool payload = nir_deref_mode_is(deref, nir_var_shader_call_data);
         deref->modes = nir_var_mem_global;
         progress = true;
         if (deref->deref_type == nir_deref_type_var) {
            b.cursor = nir_before_instr(instr);
            nir_def *base = payload ? load_field(&b, payload, 1, 64) :
                                      load_field(&b, hit_attributes, 1, 64);
            base = nir_iadd_imm(&b, base, deref->var->data.driver_location);
            nir_deref_instr *replacement = nir_build_deref_cast(&b, base,
               nir_var_mem_global, deref->var->type, 0);
            nir_def_replace(&deref->def, &replacement->def);
         }
      }
   }
   if (progress) {
      widen_derefs(shader, nir_var_mem_global);
      NIR_PASS(_, shader, nir_lower_explicit_io, nir_var_mem_global, nir_address_format_64bit_global);
   }
   return progress;
}

static bool
lower_system_value(nir_builder *b, nir_intrinsic_instr *in, void *unused)
{
   b->cursor = nir_before_instr(&in->instr);
   nir_def *value;
   switch (in->intrinsic) {
#define KK_RT_SYSVAL(op, member, n, bits) \
   case nir_intrinsic_load_##op: value = load_field(b, member, n, bits); break
   KK_RT_SYSVAL(ray_launch_id, launch_id, 3, 32);
   KK_RT_SYSVAL(ray_launch_size, launch_size, 3, 32);
   KK_RT_SYSVAL(ray_world_origin, origin, 3, 32);
   KK_RT_SYSVAL(ray_world_direction, direction, 3, 32);
   KK_RT_SYSVAL(ray_object_origin, object_origin, 3, 32);
   KK_RT_SYSVAL(ray_object_direction, object_direction, 3, 32);
   KK_RT_SYSVAL(ray_t_min, tmin, 1, 32);
   KK_RT_SYSVAL(ray_t_max, tmax, 1, 32);
   KK_RT_SYSVAL(ray_flags, flags, 1, 32);
   KK_RT_SYSVAL(cull_mask, cull_mask, 1, 32);
   KK_RT_SYSVAL(ray_geometry_index, geometry_id, 1, 32);
   KK_RT_SYSVAL(ray_instance_custom_index, instance_custom_index, 1, 32);
   KK_RT_SYSVAL(instance_id, instance_id, 1, 32);
   KK_RT_SYSVAL(primitive_id, primitive_id, 1, 32);
   KK_RT_SYSVAL(ray_hit_kind, hit_kind, 1, 32);
   KK_RT_SYSVAL(shader_record_ptr, shader_record, 1, 64);
   KK_RT_SYSVAL(subgroup_invocation, lane, 1, 32);
   KK_RT_SYSVAL(subgroup_size, subgroup_size, 1, 32);
#undef KK_RT_SYSVAL
   case nir_intrinsic_load_ray_object_to_world:
   case nir_intrinsic_load_ray_world_to_object: {
      unsigned column = nir_intrinsic_column(in);
      assert(column < 4);
      unsigned base = in->intrinsic == nir_intrinsic_load_ray_object_to_world ?
         offsetof(struct kk_rt_cps_context, object_to_world) :
         offsetof(struct kk_rt_cps_context, world_to_object);
      nir_def *rows[3];
      for (unsigned r = 0; r < 3; ++r)
         rows[r] = context_load(b, base + r * 16 + column * 4, 1, 32);
      value = nir_vec(b, rows, 3);
      break;
   }
   case nir_intrinsic_load_ray_triangle_vertex_positions: {
      value = context_load(b, offsetof(struct kk_rt_cps_context, triangle_vertices) +
                                   nir_intrinsic_column(in) * 3 * sizeof(float), 3, 32);
      break;
   }
   default:
      return false;
   }
   nir_def_replace(&in->def, value);
   return true;
}

/* Common nir_lower_shader_calls currently requires report_intersection's any
 * hit to have been inlined. Route REPORT through the same suspension/spill
 * mechanism as CALLABLE, keeping a separate explicit action in the context.
 * No application SBT index or token is reserved as a marker. The acceptance
 * result is loaded after the resume, never falsely returned before any-hit. */
static bool
prepare_call(nir_builder *b, nir_intrinsic_instr *in, void *unused)
{
   b->cursor = nir_before_instr(&in->instr);
   switch (in->intrinsic) {
   case nir_intrinsic_execute_callable:
      if (unused) return false;
      store_field(b, action, nir_imm_int(b, KK_RT_CALLABLE));
      break;
   case nir_intrinsic_report_ray_intersection: {
      store_field(b, action, nir_imm_int(b, KK_RT_REPORT));
      store_field(b, report_t, in->src[0].ssa);
      store_field(b, report_kind, in->src[1].ssa);
      nir_execute_callable(b, nir_imm_int(b, 0), nir_imm_int64(b, 0));
      nir_def *accepted = load_field(b, report_accepted, 1, 32);
      nir_def_replace(&in->def, nir_ine_imm(b, accepted, 0));
      return true;
   }
   case nir_intrinsic_ignore_ray_intersection:
   case nir_intrinsic_terminate_ray:
      store_field(b, action, nir_imm_int(b, in->intrinsic == nir_intrinsic_terminate_ray ?
                                          KK_RT_TERMINATE : KK_RT_IGNORE));
      nir_jump(b, nir_jump_halt);
      nir_instr_remove(&in->instr);
      return true;
   default:
      return false;
   }
   return true;
}

static bool
lower_part(nir_builder *b, nir_intrinsic_instr *in, void *unused)
{
   b->cursor = nir_before_instr(&in->instr);
   switch (in->intrinsic) {
   case nir_intrinsic_rt_resume:
      /* The scheduler has restored the caller frame before entering this
       * resume. Common spill loads therefore address that exact frame. */
      nir_instr_remove(&in->instr);
      return true;
   case nir_intrinsic_load_scratch_base_ptr: {
      nir_def *base = nir_iadd(b, load_field(b, stack_base, 1, 64),
                                 load_field(b, frame_base, 1, 64));
      nir_def_replace(&in->def, base);
      return true;
   }
   case nir_intrinsic_rt_trace_ray: {
      store_field(b, action, nir_imm_int(b, KK_RT_TRACE));
      const unsigned fields[] = {
         offsetof(struct kk_rt_cps_context, acceleration_structure),
         offsetof(struct kk_rt_cps_context, flags),
         offsetof(struct kk_rt_cps_context, cull_mask),
         offsetof(struct kk_rt_cps_context, sbt_offset),
         offsetof(struct kk_rt_cps_context, sbt_stride),
         offsetof(struct kk_rt_cps_context, miss_index),
         offsetof(struct kk_rt_cps_context, origin),
         offsetof(struct kk_rt_cps_context, tmin),
         offsetof(struct kk_rt_cps_context, direction),
         offsetof(struct kk_rt_cps_context, tmax),
         offsetof(struct kk_rt_cps_context, payload),
      };
      for (unsigned i = 0; i < ARRAY_SIZE(fields); ++i)
         context_store(b, fields[i], (i == 0 || i == 10) ?
            nir_u2u64(b, in->src[i].ssa) : in->src[i].ssa);
      break;
   }
   case nir_intrinsic_rt_execute_callable:
      /* prepare_call already selected REPORT or CALLABLE before suspension.
       * Report never publishes a fabricated application callable index. */
      store_field(b, callable_index, in->src[0].ssa);
      /* REPORT does not replace the active procedural shader's payload. */
      nir_push_if(b, nir_ieq_imm(b, load_field(b, action, 1, 32), KK_RT_CALLABLE));
      store_field(b, payload, nir_u2u64(b, in->src[1].ssa));
      nir_pop_if(b, NULL);
      break;
   default:
      return false;
   }
   store_field(b, resume, nir_imm_int(b, nir_intrinsic_call_idx(in) + 1));
   store_field(b, frame_size, nir_imm_int(b, nir_intrinsic_stack_size(in)));
   nir_instr_remove(&in->instr);
   return true;
}

/* Mesa's shader-call intrinsics encode individual frame byte offsets in32
 * bits. Evaluate the complete variable and conservative SSA spill bound at64
 * bits before entering those common passes; an unrepresentable allocation is
 * rejected rather than wrapping the common intermediate arithmetic. */
static bool
kk_rt_type_size(const struct glsl_type *type, uint64_t *size, uint64_t *alignment)
{
   if(glsl_type_is_array(type)){
      uint64_t element,align;if(!kk_rt_type_size(glsl_get_array_element(type),&element,&align)||
         element>UINT64_MAX-(align-1))return false;
      element=(element+align-1)&~(align-1);
      if(glsl_get_length(type)&&element>UINT64_MAX/glsl_get_length(type))return false;
      *size=element*glsl_get_length(type);*alignment=align;return true;
   }
   if(glsl_type_is_struct(type)||glsl_type_is_interface(type)){
      uint64_t total=0,max_align=1;
      for(unsigned i=0;i<glsl_get_length(type);++i){uint64_t field,align;
         if(!kk_rt_type_size(glsl_get_struct_field(type,i),&field,&align)||total>UINT64_MAX-(align-1))return false;
         total=(total+align-1)&~(align-1);if(field>UINT64_MAX-total)return false;total+=field;max_align=MAX2(max_align,align);
      }
      if(total>UINT64_MAX-(max_align-1))return false;*size=(total+max_align-1)&~(max_align-1);*alignment=max_align;return true;
   }
   unsigned n,a;glsl_get_natural_size_align_bytes(type,&n,&a);*size=n;*alignment=a;return a!=0;
}
bool
kk_rt_shader_frame_fits(nir_shader *shader)
{
   uint64_t bound=shader->scratch_size;
   const nir_variable_mode modes=nir_var_function_temp|nir_var_shader_temp|nir_var_shader_call_data|nir_var_ray_hit_attrib;
   nir_foreach_variable_with_modes(var,shader,modes){uint64_t size,align;
      if(!kk_rt_type_size(var->type,&size,&align)||size>UINT32_MAX-15||bound>UINT32_MAX-size-15)return false;bound+=size+15;
   }
   nir_foreach_function_impl(impl,shader){
      nir_foreach_function_temp_variable(var,impl){uint64_t size,align;
         if(!kk_rt_type_size(var->type,&size,&align)||size>UINT32_MAX-15||bound>UINT32_MAX-size-15)return false;bound+=size+15;
      }
      nir_foreach_block(block,impl)nir_foreach_instr(instr,block){nir_def *def=nir_instr_def(instr);if(!def)continue;
         uint64_t bytes=(uint64_t)def->num_components*MAX2(def->bit_size,32)/8+16;
         if(bound>UINT32_MAX-bytes)return false;bound+=bytes;
      }
   }
   return bound<=UINT32_MAX-16;
}

bool
kk_rt_split_shader_mode(nir_shader *shader, void *mem_ctx,
                    struct kk_rt_shader_parts *parts, bool internal)
{
   memset(parts, 0, sizeof(*parts));
   if(!kk_rt_shader_frame_fits(shader))return false;
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   assert(impl->function->num_params <= 1);

   /* The shared query lowering runs before this stage splitter. Keep that
    * ownership outside the continuation module; an unlowered query is not
    * silently converted to a different execution model. */
   nir_foreach_function_impl(query_impl, shader) {
      nir_foreach_block(block, query_impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_intrinsic_is_ray_query(nir_instr_as_intrinsic(instr)->intrinsic))
               return false;
         }
      }
   }
   if(!kk_rt_prepare_context(shader))return false;

   NIR_PASS(_, shader, nir_lower_var_copies);
   lower_call_data(shader, &parts->payload_size, &parts->hit_attribute_size);
   nir_shader_intrinsics_pass(shader, lower_system_value, nir_metadata_none, NULL);
   NIR_PASS(_, shader, nir_lower_vars_to_ssa);
   /* Invocation-private arrays that survive a call must live in the same
    * explicit frame as common SSA spills, not fresh MSL local arrays. */
   NIR_PASS(_, shader, nir_lower_vars_to_explicit_types,
      nir_var_function_temp | nir_var_shader_temp, glsl_get_natural_size_align_bytes);
   widen_derefs(shader, nir_var_function_temp | nir_var_shader_temp);
   NIR_PASS(_, shader, nir_lower_explicit_io,
      nir_var_function_temp | nir_var_shader_temp, nir_address_format_64bit_global);
   nir_shader_intrinsics_pass(shader, prepare_call, nir_metadata_none, internal ? parts : NULL);
   NIR_PASS(_, shader, nir_opt_dead_cf);

   const nir_lower_shader_calls_options options = {
      .address_format = nir_address_format_64bit_global,
      .stack_alignment = 16,
      .localized_loads = true,
   };
   parts->entry = shader;
   nir_lower_shader_calls(shader, &options, &parts->resumes, &parts->resume_count, mem_ctx);
   parts->scratch_size = shader->scratch_size;
   for (uint32_t i = 0; i <= parts->resume_count; ++i) {
      nir_shader *part = i ? parts->resumes[i - 1] : shader;
      parts->scratch_size = MAX2(parts->scratch_size, part->scratch_size);
      nir_shader_intrinsics_pass(part, lower_part, nir_metadata_none, NULL);
      NIR_PASS(_, part, nir_opt_dead_cf);
      nir_validate_shader(part, "Kosmic RT continuation");
   }
   return true;
}

bool
kk_rt_split_shader(nir_shader *shader, void *mem_ctx, struct kk_rt_shader_parts *parts)
{ return kk_rt_split_shader_mode(shader, mem_ctx, parts, false); }
