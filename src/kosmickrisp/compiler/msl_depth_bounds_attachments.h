/* SPDX-License-Identifier: MIT */
#ifndef MSL_DEPTH_BOUNDS_ATTACHMENTS_H
#define MSL_DEPTH_BOUNDS_ATTACHMENTS_H
#include "kosmickrisp/vulkan/kk_depth_bounds_types.h"
#include "kosmickrisp/vulkan/kk_interlock_envelope.h"
#include "poly/geometry.h"
#include "kosmickrisp/libkk/kk_argument_table.h"

static nir_def *
msl_depth_bounds_parameters(nir_builder *b, const struct lower_ia_state *state)
{
   nir_def *pointer = state->mesh_shadow ?
      nir_iadd_imm(b, nir_load_buffer_ptr_kk(b, 1, 64, .binding = KK_ARGUMENT_PER_DRAW),
         offsetof(struct kk_depth_bounds_capture_context, env)) :
      nir_iadd_imm(b, nir_load_geometry_param_buffer_poly(b),
         offsetof(struct poly_geometry_params, capture));
   return nir_load_global_constant(b, 1, 64, pointer, .align_mul = 8);
}

/* Use the bound command's mapping, including dynamic input indices. The
 * original descriptor read remains the fallback for unmapped source images
 * and custom resolve. No opaque native handle is interpreted as identity. */
static nir_def *
msl_depth_bounds_input(nir_builder *b, nir_intrinsic_instr *intr,
   const struct lower_ia_state *state)
{
   if (!state->depth_bounds_shadow || state->rp->custom_resolve)
      return &intr->def;
   bool ms = nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_SUBPASS_MS;
   if (!ms && nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_SUBPASS)
      return &intr->def;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *params = msl_depth_bounds_parameters(b, state);
   nir_def *index = nir_iadd_imm(b, intr->src[3].ssa, nir_intrinsic_base(intr));
   nir_def *stored[10];
   unsigned count = 0;
   for (unsigned i = 0; i < 10; ++i) {
      nir_alu_type base = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr));
      if ((i == 8 && base != nir_type_float) || (i == 9 && base == nir_type_float))
         continue;
      unsigned offset = i < 8 ?
         offsetof(struct kk_depth_bounds_envelope, input_color_indices) + i * 4 :
         i == 8 ? offsetof(struct kk_depth_bounds_envelope, input_depth_index) :
                  offsetof(struct kk_depth_bounds_envelope, input_stencil_index);
      nir_def *mapped = nir_load_global_constant(b, 1, 32,
         nir_iadd_imm(b, params, offset), .align_mul = 4);
      nir_def *matches = nir_iand(b, nir_ine_imm(b, mapped, MESA_VK_ATTACHMENT_UNUSED),
         nir_ieq(b, mapped, index));
      nir_push_if(b, matches);
      nir_io_semantics io = {.location = i < 8 ? FRAG_RESULT_DATA0 + i :
         i == 8 ? FRAG_RESULT_DEPTH : FRAG_RESULT_STENCIL, .num_slots = 1};
      stored[count++] = nir_load_tile_image(b, intr->def.num_components,
         intr->def.bit_size, nir_imm_int(b, 0),
         ms ? intr->src[2].ssa : nir_imm_int(b, 0),
         .dest_type = nir_intrinsic_dest_type(intr), .io_semantics = io,
         .access = ACCESS_COHERENT);
      nir_push_else(b, NULL);
   }
   /* Keep the original texture access inside the unmapped branch. A value
    * select would still execute it for a mapped transient/feedback image. */
   nir_intrinsic_instr *fallback = nir_instr_as_intrinsic(
      nir_instr_clone(b->shader, &intr->instr));
   nir_builder_instr_insert(b, &fallback->instr);
   nir_def *result = &fallback->def;
   while (count) {
      nir_pop_if(b, NULL);
      result = nir_if_phi(b, stored[--count], result);
   }
   return result;
}
#endif
