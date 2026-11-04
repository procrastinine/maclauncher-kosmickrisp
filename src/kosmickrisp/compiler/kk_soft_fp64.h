/* SPDX-License-Identifier: MIT */
#ifndef KK_SOFT_FP64_LOWER_H
#define KK_SOFT_FP64_LOWER_H

#include "compiler/nir/nir_softfloat.h"
#include "kk_soft_fp64_library.h"

static bool
kk_soft_fp64_scalarize(const nir_instr *instr, UNUSED const void *data)
{
   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->def.bit_size == 64 &&
       nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) == nir_type_float)
      return true;
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; ++i)
      if (alu->src[i].src.ssa->bit_size == 64 &&
          nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[i]) == nir_type_float)
         return true;
   return false;
}

static nir_def *
kk_soft_fp64_bits_call(nir_builder *b, nir_alu_instr *alu,
                      nir_function *function, const struct glsl_type *return_type,
                      bool half_input)
{
   bool flush_input = alu->op == nir_op_f2f64 && alu->src[0].src.ssa->bit_size == 32 &&
      nir_is_denorm_flush_to_zero(b->shader->info.float_controls_execution_mode, 32);
   if (!half_input && !flush_input)
      return nir_lower_softfloat_func(b, alu, function, return_type);

   nir_variable *ret = nir_local_variable_create(b->impl, return_type, "fp64_return");
   nir_variable *arg = nir_local_variable_create(b->impl, half_input ? glsl_uint_type() : glsl_float_type(), "fp64_input");
   nir_deref_instr *rd = nir_build_deref_var(b, ret), *ad = nir_build_deref_var(b, arg);
   nir_def *value = nir_u2u32(b, nir_mov_alu(b, alu->src[0], 1));
   if (flush_input)
      value = nir_bcsel(b, nir_ieq_imm(b, nir_iand_imm(b, value, 0x7f800000), 0),
                       nir_iand_imm(b, value, 0x80000000), value);
   nir_store_deref(b, ad, value, 1);
   nir_def *params[] = {&rd->def, &ad->def};
   nir_inline_function_impl(b, function->impl, params, NULL);
   return nir_load_deref(b, rd);
}

/* Equal nonzero doubles have identical bits. Only that case needs a NaN
 * test, so classifying the second operand and branching on either NaN is
 * redundant. Keep the comparison in native 32-bit integer operations. */
static nir_def *
kk_soft_fp64_equal(nir_builder *b, nir_def *a, nir_def *c)
{
   nir_def *al = nir_unpack_64_2x32_split_x(b, a);
   nir_def *ah = nir_unpack_64_2x32_split_y(b, a);
   nir_def *cl = nir_unpack_64_2x32_split_x(b, c);
   nir_def *ch = nir_unpack_64_2x32_split_y(b, c);
   nir_def *mag_hi = nir_iand_imm(b, ah, 0x7fffffff);
   nir_def *nan = nir_ior(b, nir_ugt_imm(b, mag_hi, 0x7ff00000),
      nir_iand(b, nir_ieq_imm(b, mag_hi, 0x7ff00000), nir_ine_imm(b, al, 0)));
   nir_def *same = nir_iand(b, nir_ieq(b, al, cl), nir_ieq(b, ah, ch));
   nir_def *zeros = nir_ieq_imm(b,
      nir_ior(b, nir_ior(b, al, cl), nir_ishl_imm(b, nir_ior(b, ah, ch), 1)), 0);
   return nir_ior(b, nir_iand(b, same, nir_inot(b, nan)), zeros);
}

static bool
kk_soft_fp64_exact(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   b->cursor = nir_before_instr(instr);
   b->fp_math_ctrl = alu->fp_math_ctrl;
   unsigned src_bits = alu->src[0].src.ssa->bit_size;
   if ((alu->op == nir_op_i2f64 || alu->op == nir_op_u2f64) && src_bits < 32) {
      nir_def *src = nir_mov_alu(b, alu->src[0], 1);
      nir_def *wide = alu->op == nir_op_i2f64 ? nir_i2i32(b, src) : nir_u2u32(b, src);
      nir_def *result = alu->op == nir_op_i2f64 ? nir_i2f64(b, wide) : nir_u2f64(b, wide);
      nir_def_rewrite_uses(&alu->def, result);
      nir_instr_remove(instr);
      return true;
   }
   bool half_input = alu->op == nir_op_f2f64 && src_bits == 16;
   bool single_input = alu->op == nir_op_f2f64 && src_bits == 32;
   if (src_bits != 64 && !half_input && !single_input)
      return false;
   const char *name, *mangled;
   const struct glsl_type *return_type = glsl_uint64_t_type();
   nir_def *direct = NULL;
   if (alu->op == nir_op_slt || alu->op == nir_op_sge ||
       alu->op == nir_op_seq || alu->op == nir_op_sne) {
      nir_def *a = nir_mov_alu(b, alu->src[0], 1), *c = nir_mov_alu(b, alu->src[1], 1);
      nir_def *cmp;
      switch (alu->op) {
      case nir_op_slt: cmp = nir_flt(b, a, c); break;
      case nir_op_sge: cmp = nir_fge(b, a, c); break;
      case nir_op_seq: cmp = nir_feq(b, a, c); break;
      default: cmp = nir_fneu(b, a, c); break;
      }
      direct = nir_b2f64(b, cmp);
   } else if (alu->op == nir_op_feq || alu->op == nir_op_fneu) {
      nir_def *a = nir_mov_alu(b, alu->src[0], 1);
      nir_def *c = nir_mov_alu(b, alu->src[1], 1);
      direct = kk_soft_fp64_equal(b, a, c);
      if (alu->op == nir_op_fneu)
         direct = nir_inot(b, direct);
   } else if (alu->op == nir_op_fcanonicalize) {
      direct = nir_mov_alu(b, alu->src[0], 1);
   } else if (alu->op == nir_op_fisnormal) {
      nir_def *exp = nir_iand_imm(b, nir_ushr_imm(b, nir_mov_alu(b, alu->src[0], 1), 52), 2047);
      direct = nir_iand(b, nir_ine_imm(b, exp, 0), nir_ult_imm(b, exp, 2047));
   } else if (alu->op == nir_op_flrp) {
      nir_def *a = nir_mov_alu(b, alu->src[0], 1), *c = nir_mov_alu(b, alu->src[1], 1);
      nir_def *t = nir_mov_alu(b, alu->src[2], 1);
      direct = nir_fadd(b, nir_fmul(b, a, nir_fsub(b, nir_imm_double(b, 1.0), t)), nir_fmul(b, c, t));
   }
   if (direct) {
      nir_def_rewrite_uses(&alu->def, direct);
      nir_instr_remove(instr);
      return true;
   }
   switch (alu->op) {
   case nir_op_f2f64:
      name = half_input ? "kk_half_to_f64" : "__fp32_to_fp64";
      mangled = half_input ? "kk_half_to_f64(u1;" : "__fp32_to_fp64(f1;"; break;
   case nir_op_f2f32:
      name = "__fp64_to_fp32"; mangled = "__fp64_to_fp32(u641;";
      return_type = glsl_float_type(); break;
   case nir_op_f2f16_rtz:
      name = "kk_f64_to_half_rtz"; mangled = "kk_f64_to_half_rtz(u641;";
      return_type = glsl_uint_type(); break;
   case nir_op_f2f16_rtne:
   case nir_op_f2f16:
      name = "kk_f64_to_half"; mangled = "kk_f64_to_half(u641;";
      return_type = glsl_uint_type(); break;
   case nir_op_frexp_sig:
      name = "kk_f64_frexp_sig"; mangled = "kk_f64_frexp_sig(u641;"; break;
   case nir_op_frexp_exp:
      name = "kk_f64_frexp_exp"; mangled = "kk_f64_frexp_exp(u641;";
      return_type = glsl_int_type(); break;
   case nir_op_frem:
      name = "kk_f64_rem"; mangled = "kk_f64_rem(u641;u641;"; break;
   case nir_op_fmod:
      name = "kk_f64_mod"; mangled = "kk_f64_mod(u641;u641;"; break;
   case nir_op_frsq:
      name = "kk_f64_rsq"; mangled = "kk_f64_rsq(u641;"; break;
   case nir_op_fdiv:
      name = "kk_f64_div"; mangled = "kk_f64_div(u641;u641;"; break;
   case nir_op_frcp:
      name = "kk_f64_rcp"; mangled = "kk_f64_rcp(u641;"; break;
   case nir_op_ffma:
   case nir_op_ffma_weak:
   case nir_op_fmad:
      name = "kk_f64_fma"; mangled = "kk_f64_fma(u641;u641;u641;"; break;
   case nir_op_fsqrt:
      name = "__fsqrt64"; mangled = "__fsqrt64(u641;"; break;
   case nir_op_ldexp:
      name = "kk_f64_ldexp"; mangled = "kk_f64_ldexp(u641;i1;"; break;
   case nir_op_f2i8: case nir_op_f2u8:
   case nir_op_f2i16: case nir_op_f2u16:
   case nir_op_f2i32: case nir_op_f2u32:
   case nir_op_f2i64: case nir_op_f2u64:
      name = "kk_f64_to_integer"; mangled = "kk_f64_to_integer(u641;"; break;
   default:
      return false;
   }
   const nir_shader *library = data;
   nir_function *function = nir_shader_get_function_for_name(library, name);
   if (!function)
      function = nir_shader_get_function_for_name(library, mangled);
   assert(function && function->impl);
   b->cursor = nir_before_instr(instr);
   b->fp_math_ctrl = alu->fp_math_ctrl;
   nir_def *result = kk_soft_fp64_bits_call(b, alu, function, return_type, half_input);
   if (alu->def.bit_size != result->bit_size)
      result = nir_u2uN(b, result, alu->def.bit_size);
   if (alu->op == nir_op_f2f32 &&
       nir_is_denorm_flush_to_zero(b->shader->info.float_controls_execution_mode, 32))
      result = nir_bcsel(b, nir_ieq_imm(b, nir_iand_imm(b, result, 0x7f800000), 0),
                        nir_iand_imm(b, result, 0x80000000), result);
   nir_def_rewrite_uses(&alu->def, result);
   nir_instr_remove(instr);
   return true;
}

static bool
kk_soft_fp64_subgroup(const nir_intrinsic_instr *intrin, UNUSED const void *data)
{
   switch (intrin->intrinsic) {
   case nir_intrinsic_vote_feq:
      return intrin->src[0].ssa->bit_size == 64;
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan:
      return intrin->def.bit_size == 64 &&
         nir_alu_type_get_base_type(nir_op_infos[nir_intrinsic_reduction_op(intrin)].output_type) == nir_type_float;
   default:
      return false;
   }
}

/* The pinned Mesa library represents doubles as integer bits. Its functions
 * are inlined into the same shader, then consumed by the ordinary optimizer.
 * No shader variant or additional GPU resource is needed.
 */
static void
kk_soft_fp64_lower(nir_shader *nir)
{
   if (!(nir->info.bit_sizes_float & 64))
      return;

   /* Keep the scan/reduce control flow shared across vector components.
    * Scalarize its arithmetic below, after lowering the collective once.
    * The float-size summary includes floating subgroup intrinsics. */
   const nir_lower_subgroups_options subgroups = {
      .filter = kk_soft_fp64_subgroup,
      .subgroup_size = 32, .ballot_bit_size = 32, .ballot_components = 1,
      .lower_reduce = true, .lower_vote_feq = true,
   };
   if (nir->info.uses_wide_subgroup_intrinsics)
      NIR_PASS(_, nir, nir_lower_subgroups, &subgroups);

   const nir_shader *library = kk_soft_fp64_library(nir->options);
   assert(library);

   NIR_PASS(_, nir, nir_lower_alu_to_scalar, kk_soft_fp64_scalarize, NULL);
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_shader_instructions_pass, kk_soft_fp64_exact,
               nir_metadata_none, (void *)library);
      /* Inlining copies library SSA indices. The generic lowering repairs
       * them only when it also changes an instruction. Conversion-only
       * shaders need the same repair before index-based optimizations. */
      if (progress) {
         nir_foreach_function_impl(impl, nir)
            nir_index_ssa_defs(impl);
      }
      NIR_PASS(progress, nir, nir_lower_doubles, library,
               (nir_lower_doubles_options)(~0));
      NIR_PASS(_, nir, nir_opt_deref);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
   } while (progress);
   NIR_PASS(_, nir, nir_lower_continue_constructs);
}
#endif
