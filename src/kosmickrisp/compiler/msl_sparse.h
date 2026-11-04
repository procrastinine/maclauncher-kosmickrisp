/* SPDX-License-Identifier: MIT */
#ifndef KK_SPARSE_MSL_H
#define KK_SPARSE_MSL_H

/* Residency is tied to the original texture result by NIR's existing
 * residency intrinsic. Ordinary texture loads use the same emitter. */
static bool
msl_def_has_sparse_residency(const struct nir_to_msl_ctx *ctx, nir_def *def)
{
   /* Type seeding already visits every instruction. Without a residency
    * intrinsic, no result can have a sparse-status user. */
   if (!ctx->sparse_residency) return false;
   nir_foreach_use(src, def) {
      nir_instr *use = nir_src_use_instr(src);
      if (use->type == nir_instr_type_intrinsic &&
          nir_instr_as_intrinsic(use)->intrinsic == nir_intrinsic_is_sparse_resident_zink)
         return true;
   }
   return false;
}
#endif
