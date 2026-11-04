/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAGMENT_SHADING_RATE_INPUTS_NIR_H
#define KK_FRAGMENT_SHADING_RATE_INPUTS_NIR_H
#include "kk_interlock_capture_nir.h"

/* Center planes are also needed to evaluate a logical centroid. Retaining
 * only the native centroid value would select a different original sample.
 * This layout is requested only by the actual coarse capture variant. */
static bool
kk_vrs_capture_layout(nir_shader *source, nir_function *application,
   struct kk_interlock_capture_nir *layout)
{
   if (!source || !application || !application->impl || !layout)
      return false;
   kk_interlock_capture_collect_layout(source, layout);
   uint64_t body_centroid = 0, fine_centroid = 0;
   nir_foreach_function_impl(impl, source) nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_interpolated_input ||
             nir_src_as_intrinsic(intr->src[0])->intrinsic != nir_intrinsic_load_barycentric_centroid)
            continue;
         unsigned slot = nir_intrinsic_io_semantics(intr).location + nir_src_as_uint(intr->src[1]);
         if (impl == application->impl)
            body_centroid |= BITFIELD64_BIT(slot);
         else
            fine_centroid |= BITFIELD64_BIT(slot);
      }
   layout->center |= body_centroid;
   layout->interpolants |= body_centroid;
   layout->centroid = fine_centroid;
   kk_interlock_capture_finish_layout(layout);
   return true;
}

struct kk_vrs_captured_inputs_nir {
   /* coord names the actual captured anchor, not the coarse center. The
    * record callback returns that anchor's immutable original quad packet. */
   struct kk_interlock_captured_inputs_nir capture;
   nir_variable *center;   /* absolute logical FragCoord, float4 */
   nir_variable *centroid; /* absolute selected covered sample, float2 */
};

static bool
kk_vrs_captured_target(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct kk_vrs_captured_inputs_nir *s = data;
   if (intr->intrinsic == nir_intrinsic_load_point_coord && s->capture.capture->point_coord) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_def *record = s->capture.record(b, s->capture.data, nir_imm_int(b, 0));
      nir_def *anchor = nir_ffloor(b, nir_channels(b,
         nir_load_var(b, s->capture.coord), 3));
      nir_def *offset = nir_fsub(b, nir_channels(b, nir_load_var(b, s->center), 3),
         nir_fadd_imm(b, anchor, .5f));
      nir_def *base_offset = s->capture.capture->point_sample ?
         nir_fadd_imm(b, s->capture.sample_position(b, s->capture.data,
            nir_imm_int(b, 0)), -.5f) : nir_imm_vec2(b, 0, 0);
      nir_def *value = kk_interlock_captured_offset(b, record,
         nir_iadd_imm(b, record, s->capture.capture->stride + 32), s->capture.capture,
         8, 2, nir_f2u32(b, anchor), offset, base_offset, false);
      nir_def_replace(&intr->def, value);
      return true;
   }
   if (intr->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;
   nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
   bool centroid = bary->intrinsic == nir_intrinsic_load_barycentric_centroid;
   bool offset = bary->intrinsic == nir_intrinsic_load_barycentric_at_offset;
   if (!centroid && !offset && bary->intrinsic != nir_intrinsic_load_barycentric_pixel)
      return false; /* Original explicit sample indices keep their reader. */
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *target = centroid ? nir_load_var(b, s->centroid) :
      nir_channels(b, nir_load_var(b, s->center), 3);
   if (offset)
      target = nir_fadd(b, target, bary->src[0].ssa);
   nir_def *anchor = nir_fadd_imm(b, nir_ffloor(b,
      nir_channels(b, nir_load_var(b, s->capture.coord), 3)), .5f);
   nir_src_rewrite(&intr->src[0], nir_load_barycentric_at_offset(b, 32,
      nir_fsub(b, target, anchor), .interp_mode = nir_intrinsic_interp_mode(bary)));
   return true;
}

/* Borrow the existing input reader/typed plane layout. Neither this pass nor
 * the shared reader runs an application stage or synthesizes new geometry.
 * The fine epilogue is deliberately excluded from these input rewrites. */
static bool
kk_vrs_lower_captured_inputs(nir_shader *shader, nir_function *application,
   const struct kk_vrs_captured_inputs_nir *inputs)
{
   if (!shader || shader->info.stage != MESA_SHADER_FRAGMENT ||
       !application || !application->impl || !inputs || !inputs->center ||
       !inputs->centroid || !inputs->capture.capture || !inputs->capture.coord ||
       !inputs->capture.sample || !inputs->capture.record || !inputs->capture.sample_position)
      return false;
   const struct kk_interlock_capture_nir *layout = inputs->capture.capture;
   nir_foreach_block(block, application->impl) nir_foreach_instr(instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_load_interpolated_input)
         continue;
      nir_intrinsic_op bary = nir_src_as_intrinsic(intr->src[0])->intrinsic;
      unsigned slot = nir_intrinsic_io_semantics(intr).location + nir_src_as_uint(intr->src[1]);
      if ((bary == nir_intrinsic_load_barycentric_pixel ||
           bary == nir_intrinsic_load_barycentric_centroid ||
           bary == nir_intrinsic_load_barycentric_at_offset) &&
          !(layout->center & BITFIELD64_BIT(slot)))
         return false;
   }
   nir_function_intrinsics_pass(application->impl, kk_vrs_captured_target,
      nir_metadata_none, (void *)inputs);
   nir_function_intrinsics_pass(application->impl, kk_interlock_captured_input,
      nir_metadata_none, (void *)&inputs->capture);
   return true;
}
#endif
