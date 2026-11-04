/* SPDX-License-Identifier: MIT
 * An explicit ROG resource brackets arbitrary storage addresses. SPIR-V already
 * supplies device/image acquire and release fences at the interlock boundaries.
 */
#include "kosmickrisp/libkk/kk_argument_table.h"

static void src_to_msl(struct nir_to_msl_ctx *ctx, nir_src *src);

static bool
msl_interlock_shader(const nir_shader *shader)
{
   return shader->info.stage == MESA_SHADER_FRAGMENT &&
      (shader->info.fs.pixel_interlock_ordered ||
       shader->info.fs.pixel_interlock_unordered ||
       shader->info.fs.sample_interlock_ordered ||
       shader->info.fs.sample_interlock_unordered);
}

static void
msl_interlock_prepare(nir_shader *shader)
{
   if (!msl_interlock_shader(shader))
      return;
   BITSET_SET(shader->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
   if (shader->info.fs.uses_sample_shading)
      BITSET_SET(shader->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID);
}

static void
msl_interlock_inputs(struct nir_to_msl_ctx *ctx)
{
   if (ctx->interlock)
      P_IND(ctx, "device atomic_uint *kk_rog [[buffer(%u), raster_order_group(0)]],\n",
            KK_ARGUMENT_INTERLOCK_GUARD);
}

static void
msl_interlock_locals(struct nir_to_msl_ctx *ctx)
{
   if (!ctx->interlock)
      return;
   P_IND(ctx, "device atomic_uint *kk_rog_pixel = kk_rog + (((uint(gl_FragCoord.y) & 31u) * 32u + (uint(gl_FragCoord.x) & 31u)) * 32u");
   if (BITSET_TEST(ctx->shader->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID))
      P(ctx, " + gl_SampleID");
   P(ctx, ");\n");
   P_IND(ctx, "uint kk_rog_token = 0; bool kk_rog_active = false;\n");
   P_IND(ctx, "bool kk_rog_live = !simd_is_helper_thread();\n");
}

static bool
msl_interlock_side_effect(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *instr)
{
   if (!ctx->interlock)
      return false;
   switch (instr->intrinsic) {
   case nir_intrinsic_store_global:
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
   case nir_intrinsic_bindless_image_store:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
      return true;
   default:
      return false;
   }
}

static void
msl_interlock_release(struct nir_to_msl_ctx *ctx, bool fence)
{
   P_IND(ctx, "if (kk_rog_active) {\n");
   ctx->indentlevel++;
   if (fence)
      P_IND(ctx, "atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_texture, memory_order_seq_cst, thread_scope_device);\n");
   P_IND(ctx, "atomic_store_explicit(kk_rog_pixel, kk_rog_token + 1, memory_order_relaxed);\n");
   P_IND(ctx, "kk_rog_active = false;\n");
   ctx->indentlevel--;
   P_IND(ctx, "}\n");
}

static bool
msl_interlock_intrinsic(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *instr)
{
   if (!ctx->interlock)
      return false;
   switch (instr->intrinsic) {
   case nir_intrinsic_begin_invocation_interlock:
      P_IND(ctx, "kk_rog_token = atomic_load_explicit(kk_rog_pixel, memory_order_relaxed);\n");
      P_IND(ctx, "kk_rog_active = true;\n");
      return true;
   case nir_intrinsic_end_invocation_interlock:
      msl_interlock_release(ctx, false);
      return true;
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate:
   case nir_intrinsic_terminate_if: {
      bool conditional = instr->intrinsic == nir_intrinsic_demote_if ||
                         instr->intrinsic == nir_intrinsic_terminate_if;
      bool terminate = instr->intrinsic == nir_intrinsic_terminate ||
                       instr->intrinsic == nir_intrinsic_terminate_if;
      if (conditional) {
         P_IND(ctx, "if (");
         src_to_msl(ctx, &instr->src[0]);
         P(ctx, ") {\n");
         ctx->indentlevel++;
      }
      /* Discard implicitly exits the region before helper writes are disabled. */
      msl_interlock_release(ctx, true);
      P_IND(ctx, "kk_rog_live = false;\n");
      P_IND(ctx, "discard_fragment();\n");
      if (terminate)
         P_IND(ctx, "return {};\n");
      if (conditional) {
         ctx->indentlevel--;
         P_IND(ctx, "}\n");
      }
      return true;
   }
   default:
      return false;
   }
}
