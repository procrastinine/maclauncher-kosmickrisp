/* SPDX-License-Identifier: MIT */
#ifndef KK_AS_MATH_H
#define KK_AS_MATH_H
#include <string.h>

/* AS bounds, inactive NaNs and intersection arithmetic have defined non-finite
 * and contraction behavior. This uses the existing common fp-control pipeline;
 * ordinary libkk entries retain their original numeric modes. */
static void
kk_as_preserve_kernel_math(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      if (strncmp(impl->function->name, "kk_as_", 6) != 0)
         continue;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            alu->fp_math_ctrl |= nir_op_infos[alu->op].valid_fp_math_ctrl &
                                 nir_fp_no_fast_math;
         }
      }
   }
}
#endif
