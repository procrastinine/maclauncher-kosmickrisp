/* SPDX-License-Identifier: MIT */
#ifndef KK_MESH_NIR_H
#define KK_MESH_NIR_H

/* All emission sites must be bounded independently. Multiplication is done in
 * uint64_t after testing each factor, so a 32-bit wrap cannot prove a bound. */
static uint32_t
kk_task_grid_bound(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_TASK);
   struct hash_table *ranges = _mesa_pointer_hash_table_create(NULL);
   bool bounded = true;
   uint32_t maximum = 0;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_launch_mesh_workgroups)
               continue;
            uint32_t bound[3];
            for (unsigned i = 0; i < 3; ++i)
               bound[i] = nir_unsigned_upper_bound(nir, ranges,
                  nir_get_scalar(intr->src[0].ssa, i));
            if (!(bound[0] && bound[1] && bound[2]))
               continue;
            uint64_t total = 1;
            for (unsigned i = 0; i < 3; ++i) {
               if (bound[i] > KK_MESH_MAX_GROUPS ||
                   total > KK_MESH_MAX_GROUPS / bound[i]) {
                  bounded = false;
                  break;
               }
               total *= bound[i];
            }
            if (bounded)
               maximum = MAX2(maximum, total);
         }
      }
   }
   _mesa_hash_table_destroy(ranges, NULL);
   return bounded ? maximum : KK_MESH_MAX_GROUPS;
}

static void
kk_lower_task_payload(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_TASK &&
       nir->info.stage != MESA_SHADER_MESH)
      return;
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_task_payload,
            shared_var_info);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_task_payload,
            nir_address_format_32bit_offset);
   if (nir->info.stage == MESA_SHADER_TASK) {
      NIR_PASS(_, nir, nir_lower_task_shader,
         ((nir_lower_task_shader_options){
            .payload_to_shared_for_atomics = true,
            .payload_to_shared_for_small_types = true,
         }));
   }
}

#endif
