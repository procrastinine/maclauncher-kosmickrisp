/* SPDX-License-Identifier: MIT */
#ifndef KK_ATOMIC_INT64_H
#define KK_ATOMIC_INT64_H
#include "kk_atomic_int64_msl.h"
#include "kk_atomic_int64_usage.h"

static inline unsigned
kk_atomic64_opcode(nir_atomic_op atomic_op)
{
   unsigned op;
      switch (atomic_op) {
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
   return op;
}

static inline bool
kk_emit_image64(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *intr)
{
   if (!kk_is_image64(intr)) return false;
   const bool store = intr->intrinsic == nir_intrinsic_bindless_image_store;
   const bool load = intr->intrinsic == nir_intrinsic_bindless_image_load;
   const bool swap = intr->intrinsic == nir_intrinsic_bindless_image_atomic_swap;
   const bool atomic = kk_is_image_atomic64(intr);
   const bool guarded = msl_interlock_side_effect(ctx, intr);
   if (guarded) { P_IND(ctx, "if (kk_rog_live) {\n"); ctx->indentlevel++; }
   if (!store) {
      if (swap) { P_IND(ctx, "%s t%d = ", msl_type_for_def(ctx->types, &intr->def), intr->def.index); }
      else { P_IND(ctx, "t%d = ", intr->def.index); }
      P(ctx, "as_type<%s>(", msl_type_for_def(ctx->types, &intr->def));
   } else { P_INDENT(ctx); }
   /* Without ulong textures the RG32Uint texel holds the low and high words. */
   const bool words = !ctx->native_atomic64_minmax;
   if (!atomic) {
      if (words && !store) P(ctx, "ulong4(as_type<ulong>(");
      src_to_msl(ctx, &intr->src[0]);
      if (store) {
         if (words) {
            P(ctx, ".write(uint4(as_type<uint2>(as_type<ulong4>(");
            src_to_msl(ctx, &intr->src[3]); P(ctx, ").x), 0u, 0u), ");
         } else {
            P(ctx, ".write(as_type<ulong4>("); src_to_msl(ctx, &intr->src[3]);
            P(ctx, "), ");
         }
         image_coord_swizzle(ctx, intr);
         if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF) {
            P(ctx, ", "); src_to_msl(ctx, &intr->src[4]);
         }
         P(ctx, ");\n");
      } else {
         P(ctx, ".read("); image_coord_swizzle(ctx, intr);
         if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_BUF) {
            P(ctx, ", "); src_to_msl(ctx, &intr->src[3]);
         }
         P(ctx, words ? ").xy), 0ul, 0ul, 1ul)." : ").");
         for (unsigned c = 0; c < intr->def.num_components; c++) P(ctx, "%c", "xyzw"[c]);
         P(ctx, ");\n");
      }
   } else {
      if (ctx->shader->info.stage == MESA_SHADER_FRAGMENT) {
         if (store) { P(ctx, "if (!simd_is_helper_thread()) "); }
         else { P(ctx, "simd_is_helper_thread() ? 0ul : "); }
      }
      const bool array = nir_intrinsic_image_array(intr);
      P(ctx, "kk_image_atomic64%s(", array ? "_array" : "");
      src_to_msl(ctx, &intr->src[0]); P(ctx, ", ");
      image_coord_swizzle(ctx, intr); P(ctx, ", uint2(");
      src_to_msl(ctx, &intr->src[1]);
      P(ctx, nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_1D ? ".x, 0u)" : ".xy)");
      const unsigned op = load ? 9 : store ? 8 : kk_atomic64_opcode(nir_intrinsic_atomic_op(intr));
      P(ctx, ", (device atomic_uint*)kk_atomic64_lock_table, %uu, ", op);
      if (load) { P(ctx, "0ul"); }
      else {
         P(ctx, "ulong("); src_to_msl(ctx, &intr->src[swap ? 4 : 3]);
         if (store && intr->src[3].ssa->num_components > 1) P(ctx, ".x");
         P(ctx, ")");
      }
      P(ctx, ", ");
      if (swap) { P(ctx, "ulong("); src_to_msl(ctx, &intr->src[3]); P(ctx, ")"); }
      else { P(ctx, "0ul"); }
      P(ctx, ", %s)", store || nir_def_is_unused(&intr->def) ? "true" : "false");
      if (!store) P(ctx, ")");
      P(ctx, ";\n");
   }
   if (guarded) { ctx->indentlevel--; P_IND(ctx, "}\n"); }
   return true;
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
      op = kk_atomic64_opcode(nir_intrinsic_atomic_op(intr));
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
   P(ctx, ", (device atomic_uint*)kk_atomic64_lock_table, %uu, ", op);
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
