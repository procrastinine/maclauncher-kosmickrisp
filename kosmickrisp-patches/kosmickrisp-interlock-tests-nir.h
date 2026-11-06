/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_TESTS_NIR_H
#define KK_INTERLOCK_TESTS_NIR_H

#include "compiler/nir/nir_builder.h"
#include <vulkan/vulkan_core.h>

/* Build the depth/stencil transaction in NIR, sharing the driver's ordinary
 * MSL arithmetic lowering rather than maintaining a second shader language. */
struct kk_interlock_stencil_nir {
   nir_def *reference;
   nir_def *compare_mask;
   nir_def *write_mask;
   nir_def *compare;
   nir_def *fail;
   nir_def *depth_fail;
   nir_def *pass;
};

struct kk_interlock_tests_nir {
   nir_def *depth_enable;
   nir_def *depth_write;
   nir_def *depth_compare;
   nir_def *bounds_enable;
   nir_def *bounds_min;
   nir_def *bounds_max;
   nir_def *stencil_enable;
   struct kk_interlock_stencil_nir stencil;
};

struct kk_interlock_test_result_nir {
   nir_def *passed;
   nir_def *depth;
   nir_def *stencil;
};

static nir_def *
kk_interlock_compare_nir(nir_builder *b, nir_def *op, nir_def *source,
                         nir_def *destination, bool floating)
{
   nir_def *less = floating ? nir_flt(b, source, destination) :
                              nir_ult(b, source, destination);
   nir_def *equal = floating ? nir_feq(b, source, destination) :
                               nir_ieq(b, source, destination);
   nir_def *greater = floating ? nir_flt(b, destination, source) :
                                 nir_ult(b, destination, source);
   nir_def *results[] = {
      nir_imm_false(b), less, equal, nir_ior(b, less, equal),
      greater, nir_inot(b, equal), nir_ior(b, greater, equal),
      nir_imm_true(b),
   };
   static_assert(VK_COMPARE_OP_NEVER == 0 && VK_COMPARE_OP_ALWAYS == 7,
                 "Vulkan comparison operation order");
   nir_def *value = results[0];
   for (unsigned i = 1; i < ARRAY_SIZE(results); ++i)
      value = nir_bcsel(b, nir_ieq_imm(b, op, i), results[i], value);
   return value;
}

static nir_def *
kk_interlock_stencil_op_nir(nir_builder *b, nir_def *op, nir_def *value,
                            nir_def *reference)
{
   nir_def *increment = nir_iadd_imm(b, value, 1);
   nir_def *decrement = nir_iadd_imm(b, value, -1);
   nir_def *results[] = {
      value,
      nir_imm_int(b, 0),
      reference,
      nir_umin(b, increment, nir_imm_int(b, 255)),
      nir_bcsel(b, nir_ieq_imm(b, value, 0), value, decrement),
      nir_inot(b, value),
      increment,
      decrement,
   };
   static_assert(VK_STENCIL_OP_KEEP == 0 &&
                 VK_STENCIL_OP_DECREMENT_AND_WRAP == 7,
                 "Vulkan stencil operation order");
   nir_def *result = results[0];
   for (unsigned i = 1; i < ARRAY_SIZE(results); ++i)
      result = nir_bcsel(b, nir_ieq_imm(b, op, i), results[i], result);
   return nir_iand_imm(b, result, 255);
}

/* Call before the program for early tests, or after the program for late
 * tests. Coverage includes the original sample mask and, for late tests, the
 * program's discard/output coverage. A bounds rejection has no stencil op. */
static struct kk_interlock_test_result_nir
kk_interlock_tests_nir(nir_builder *b,
                        const struct kk_interlock_tests_nir *state,
                        nir_def *coverage, nir_def *source_depth,
                        nir_def *old_depth, nir_def *old_stencil)
{
   nir_def *bounds = nir_ior(b, nir_inot(b, state->bounds_enable),
      nir_iand(b, nir_fge(b, old_depth, state->bounds_min),
                    nir_fge(b, state->bounds_max, old_depth)));
   nir_def *active = nir_iand(b, coverage, bounds);
   const struct kk_interlock_stencil_nir *st = &state->stencil;
   nir_def *reference = nir_iand_imm(b,
      nir_iand(b, st->reference, st->compare_mask), 255);
   nir_def *stored = nir_iand(b, old_stencil, st->compare_mask);
   nir_def *stencil_pass = nir_ior(b, nir_inot(b, state->stencil_enable),
      kk_interlock_compare_nir(b, st->compare, reference, stored, false));
   nir_def *depth_pass = nir_ior(b, nir_inot(b, state->depth_enable),
      kk_interlock_compare_nir(b, state->depth_compare,
                               source_depth, old_depth, true));
   nir_def *passed = nir_iand(b, active,
                              nir_iand(b, stencil_pass, depth_pass));
   nir_def *operation = nir_bcsel(b, stencil_pass,
      nir_bcsel(b, depth_pass, st->pass, st->depth_fail), st->fail);
   nir_def *new_stencil = kk_interlock_stencil_op_nir(
      b, operation, old_stencil, st->reference);
   new_stencil = nir_ior(b, nir_iand(b, new_stencil, st->write_mask),
      nir_iand(b, old_stencil, nir_inot(b, st->write_mask)));
   new_stencil = nir_bcsel(b, nir_iand(b, active, state->stencil_enable),
                             new_stencil, old_stencil);
   nir_def *write_depth = nir_iand(b, passed,
      nir_iand(b, state->depth_enable, state->depth_write));
   return (struct kk_interlock_test_result_nir) {
      .passed = passed,
      .depth = nir_bcsel(b, write_depth, source_depth, old_depth),
      .stencil = new_stencil,
   };
}

#endif
