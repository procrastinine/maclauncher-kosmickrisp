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
kk_nir_lower_polygon_scalar_outputs(nir_shader *nir)
{
   /* Poly has already clipped and culled the captured primitive. Only split
    * stores for Metal's scalar interface members; keep their slot semantics. */
   return nir_lower_io_to_scalar(nir, nir_var_shader_out,
                                 kk_polygon_scalar_output, NULL);
}
