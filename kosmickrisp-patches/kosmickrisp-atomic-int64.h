/* SPDX-License-Identifier: MIT */
#ifndef KK_ATOMIC_INT64_H
#define KK_ATOMIC_INT64_H
#include "kk_atomic_int64_msl.h"

static inline bool
kk_is_atomic64(nir_intrinsic_instr *intr)
{
   bool store = false;
   switch (intr->intrinsic) {
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      return intr->def.bit_size == 64;
   case nir_intrinsic_store_global:
   case nir_intrinsic_store_shared: store = true; FALLTHROUGH;
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_shared:
      return (store ? intr->src[0].ssa->bit_size : intr->def.bit_size) == 64 &&
             (nir_intrinsic_access(intr) & ACCESS_ATOMIC);
   default: return false;
   }
}
static inline bool
kk_uses_atomic64(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_intrinsic &&
                kk_is_atomic64(nir_instr_as_intrinsic(instr)))
               return true;
   return false;
}
static inline bool
kk_emit_atomic64(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *intr)
{
   if (!kk_is_atomic64(intr)) return false;
   bool shared = intr->intrinsic == nir_intrinsic_shared_atomic ||
      intr->intrinsic == nir_intrinsic_shared_atomic_swap ||
      intr->intrinsic == nir_intrinsic_load_shared ||
      intr->intrinsic == nir_intrinsic_store_shared;
   bool store = intr->intrinsic == nir_intrinsic_store_global ||
                intr->intrinsic == nir_intrinsic_store_shared;
   bool load = intr->intrinsic == nir_intrinsic_load_global ||
               intr->intrinsic == nir_intrinsic_load_shared;
   bool swap = intr->intrinsic == nir_intrinsic_global_atomic_swap ||
               intr->intrinsic == nir_intrinsic_shared_atomic_swap;
   unsigned op = 9;
   if (store) op = 8;
   else if (!load) {
      switch (nir_intrinsic_atomic_op(intr)) {
      case nir_atomic_op_iadd: op = 0; break;
      case nir_atomic_op_imin: op = 1; break;
      case nir_atomic_op_umin: op = 2; break;
      case nir_atomic_op_imax: op = 3; break;
      case nir_atomic_op_umax: op = 4; break;
      case nir_atomic_op_iand: op = 5; break;
      case nir_atomic_op_ior: op = 6; break;
      case nir_atomic_op_ixor: op = 7; break;
      case nir_atomic_op_xchg: op = 8; break;
      case nir_atomic_op_cmpxchg: op = 10; break;
      default: UNREACHABLE("Unsupported integer atomic");
      }
   }
   if (!store) {
      if (swap) { P_IND(ctx, "%s t%d = ", msl_type_for_def(ctx->types, &intr->def), intr->def.index); }
      else { P_IND(ctx, "t%d = ", intr->def.index); }
   }
   else { P_INDENT(ctx); }
   if (!store) P(ctx, "as_type<%s>(", msl_type_for_def(ctx->types, &intr->def));
   /* Helpers must not spin on device atomics whose side effects are suppressed. */
   if (ctx->shader->info.stage == MESA_SHADER_FRAGMENT) {
      if (store) { P(ctx, "if (!simd_is_helper_thread()) "); }
      else { P(ctx, "simd_is_helper_thread() ? 0ul : "); }
   }
   P(ctx, "kk_atomic64((%s ulong*)", shared ? "threadgroup" : "device");
   if (shared) P(ctx, "&shared_data[%u + ", nir_intrinsic_base(intr));
   src_to_msl(ctx, &intr->src[store ? 1 : 0]);
   if (shared) P(ctx, "]");
   P(ctx, ", (device atomic_uint*)buf0.contents[0], %uu, ", op);
   if (load) { P(ctx, "0ul"); }
   else {
      P(ctx, "ulong(");
      src_to_msl(ctx, &intr->src[store ? 0 : swap ? 2 : 1]);
      P(ctx, ")");
   }
   P(ctx, ", ");
   if (swap) {P(ctx, "ulong("); src_to_msl(ctx, &intr->src[1]); P(ctx, ")");}
   else { P(ctx, "0ul"); }
   P(ctx, ", %s)", store || nir_def_is_unused(&intr->def) ? "true" : "false");
   if (!store) P(ctx, ")");
   P(ctx, ";\n");
   return true;
}
#endif
