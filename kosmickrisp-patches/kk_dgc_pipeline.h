/* SPDX-License-Identifier: MIT */
#ifndef KK_DGC_PIPELINE_H
#define KK_DGC_PIPELINE_H
/* Metal's indirect uniform ABI cannot compile dependent descriptor loads.
 * Keep these shaders on the same generated-root draw replay as Poly stages. */
static bool kk_dgc_shader_needs_replay(const nir_shader *nir)
{
   if (nir->info.num_ubos || nir->info.num_ssbos || nir->info.num_textures ||
       nir->info.num_images)
      return true;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_deref &&
                (nir_instr_as_deref(instr)->modes & nir_var_mem_global))
               return true;
            if (instr->type != nir_instr_type_intrinsic) continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            const char *name = nir_intrinsic_infos[intr->intrinsic].name;
            if ((!strncmp(name, "load_global", 11) && nir_intrinsic_has_align_mul(intr)) ||
                !strncmp(name, "store_global", 12) ||
                !strncmp(name, "global_atomic", 13))
               return true;
         }
      }
   }
   return false;
}
#endif
