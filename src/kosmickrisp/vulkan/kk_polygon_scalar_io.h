/* SPDX-License-Identifier: MIT */

static bool
kk_polygon_scalar_output(const nir_intrinsic_instr *intr, UNUSED const void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   return location == VARYING_SLOT_CLIP_DIST0 ||
          location == VARYING_SLOT_CLIP_DIST1 ||
          location == VARYING_SLOT_CULL_DIST0 ||
          location == VARYING_SLOT_CULL_DIST1 ||
          location == VARYING_SLOT_CULL_PRIMITIVE ||
          location == VARYING_SLOT_CULL_PRIMITIVE + 1;
}

static bool
kk_nir_lower_polygon_scalar_outputs(nir_shader *nir, uint64_t outputs)
{
   const uint64_t scalar = VARYING_BIT_CLIP_DIST0 | VARYING_BIT_CLIP_DIST1 |
      VARYING_BIT_CULL_DIST0 | VARYING_BIT_CULL_DIST1 |
      BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE) |
      BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE + 1);
   if (!(outputs & scalar))
      return false;

   /* Poly has already clipped and culled the captured primitive. Only split
    * stores for Metal's scalar interface members; keep their slot semantics. */
   return nir_lower_io_to_scalar(nir, nir_var_shader_out,
                                 kk_polygon_scalar_output, NULL);
}
