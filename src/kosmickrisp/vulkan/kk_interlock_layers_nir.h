/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_LAYERS_NIR_H
#define KK_INTERLOCK_LAYERS_NIR_H

#include "compiler/nir/nir_builder.h"
#include "poly/geometry.h"
#include "kk_interlock_envelope.h"

/* The recorder runs original pre-raster stages once for each required Vulkan
 * view. Compute stages read their source segment's immutable view instead of
 * consuming a Metal vertex-amplification attribute. */
static nir_def *
kk_interlock_source_view_field(nir_builder *b, unsigned offset)
{
   nir_def *geometry = nir_load_geometry_param_buffer_poly(b);
   nir_def *params = nir_load_global_constant(b, 1, 64,
      nir_iadd_imm(b, geometry, offsetof(struct poly_geometry_params, capture)),
      .align_mul = 8);
   return nir_load_global_constant(b, 1, 32,
      nir_iadd_imm(b, params, offset), .align_mul = 4);
}

static bool
kk_interlock_source_view_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                                     void *data)
{
   b->cursor = nir_before_instr(&intr->instr);
   if (intr->intrinsic == nir_intrinsic_load_view_index ||
       intr->intrinsic == nir_intrinsic_load_amplification_id_kk) {
      unsigned offset = intr->intrinsic == nir_intrinsic_load_view_index ?
         offsetof(struct kk_interlock_envelope, source_view) :
         offsetof(struct kk_interlock_envelope, source_view_ordinal);
      nir_def_replace(&intr->def, kk_interlock_source_view_field(b, offset));
      return true;
   }
   if (b->shader->info.stage != MESA_SHADER_GEOMETRY)
      return false;
   if (intr->intrinsic == nir_intrinsic_store_output &&
       nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_LAYER) {
      /* Native amplification supplies the target-layer offset on the
       * ordinary raster path; selected captures write that view explicitly. */
      nir_instr_remove(&intr->instr);
      return true;
   }
   if (intr->intrinsic != nir_intrinsic_emit_vertex &&
       intr->intrinsic != nir_intrinsic_emit_vertex_with_counter)
      return false;
   nir_store_output(b, kk_interlock_source_view_field(b,
      offsetof(struct kk_interlock_envelope, source_view)), nir_imm_int(b, 0),
      .src_type = nir_type_uint32, .write_mask = 1,
      .io_semantics = {.location = VARYING_SLOT_LAYER, .num_slots = 1,
         .gs_streams = nir_intrinsic_stream_id(intr)});
   b->shader->info.outputs_written |= VARYING_BIT_LAYER;
   return true;
}

static bool
kk_interlock_lower_source_views(nir_shader *shader)
{
   assert(shader->info.stage != MESA_SHADER_FRAGMENT);
   bool progress = nir_shader_intrinsics_pass(shader,
      kk_interlock_source_view_intrinsic, nir_metadata_control_flow, NULL);
   BITSET_CLEAR(shader->info.system_values_read, SYSTEM_VALUE_AMPLIFICATION_ID_KK);
   BITSET_CLEAR(shader->info.system_values_read, SYSTEM_VALUE_VIEW_INDEX);
   return progress;
}

#endif
