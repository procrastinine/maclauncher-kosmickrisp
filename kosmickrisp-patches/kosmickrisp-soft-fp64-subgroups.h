/*
 * Copyright © 2023 Collabora, Ltd.
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
/* The pinned masked scan algorithm is shared by both paths. Software binary64
 * uses one bounded GPU loop rather than cloning its arithmetic five times.
 * Native widths retain the original unrolled lowering. */
static nir_def *
build_scan_reduce(nir_builder *b, nir_intrinsic_op op, nir_op red_op,
                  nir_def *data, nir_def *mask, unsigned max_mask_bits,
                  const nir_lower_subgroups_options *options)
{
   bool unknown_size = !options->subgroup_size;
   nir_def *subgroup_size = unknown_size ? nir_load_subgroup_size(b) : NULL;

   nir_def *lt_mask = nir_load_subgroup_lt_mask(b, options->ballot_components,
                                                options->ballot_bit_size);

   /* Mask of all channels whose values we need to accumulate.  Our own value
    * is already in accum, if inclusive, thanks to the initialization above.
    * We only need to consider lower indexed invocations.
    */
   nir_def *remaining = nir_iand(b, mask, lt_mask);

   bool compact = options->subgroup_size == 32 && data->bit_size == 64 &&
      nir_alu_type_get_base_type(nir_op_infos[red_op].output_type) == nir_type_float;
   nir_variable *values = NULL, *pending = NULL, *iteration = NULL;
   if (compact) {
      values = nir_local_variable_create(b->impl,
         glsl_vector_type(GLSL_TYPE_UINT64, data->num_components), "fp64_scan");
      pending = nir_local_variable_create(b->impl, glsl_uint_type(), "fp64_pending");
      iteration = nir_local_variable_create(b->impl, glsl_uint_type(), "fp64_step");
      nir_store_var(b, values, data, ~0);
      nir_store_var(b, pending, remaining, 1);
      nir_store_var(b, iteration, nir_imm_int(b, 1), 1);
   }

   for (unsigned i = 1; i < max_mask_bits; i *= 2) {
      nir_def *step = NULL;
      if (compact) {
         nir_loop *loop = nir_push_loop(b);
         loop->control = nir_loop_control_dont_unroll;
         step = nir_load_var(b, iteration);
         nir_push_if(b, nir_uge_imm(b, step, max_mask_bits));
         nir_jump(b, nir_jump_break);
         nir_pop_if(b, NULL);
         data = nir_load_var(b, values);
         remaining = nir_load_var(b, pending);
      }
      nir_def *old_data = data;
      nir_def *old_remaining = remaining;

      if (unknown_size)
         nir_push_if(b, nir_ugt_imm(b, subgroup_size, i));

      /* At each step, our buddy channel is the first channel we have yet to
       * take into account in the accumulator.
       */
      nir_def *has_buddy = nir_bany_inequal(b, remaining, nir_imm_int(b, 0));
      nir_def *buddy = nir_ballot_find_msb(b, remaining);

      /* Accumulate with our buddy channel, if any */
      nir_def *buddy_data = nir_shuffle(b, data, buddy);
      nir_def *accum = nir_build_alu2(b, red_op, data, buddy_data);
      data = nir_bcsel(b, has_buddy, accum, data);

      /* We just took into account everything in our buddy's accumulator from
       * the previous step.  The only things remaining are whatever channels
       * were remaining for our buddy.
       */
      nir_def *buddy_remaining = nir_shuffle(b, remaining, buddy);
      remaining = nir_bcsel(b, has_buddy, buddy_remaining, nir_imm_int(b, 0));

      if (unknown_size) {
         nir_pop_if(b, NULL);
         data = nir_if_phi(b, data, old_data);
         remaining = nir_if_phi(b, remaining, old_remaining);
      }
      if (compact) {
         nir_store_var(b, values, data, ~0);
         nir_store_var(b, pending, remaining, 1);
         nir_store_var(b, iteration, nir_ishl_imm(b, step, 1), 1);
         nir_pop_loop(b, NULL);
         data = nir_load_var(b, values);
         break;
      }
   }

   switch (op) {
   case nir_intrinsic_exclusive_scan: {
      /* For exclusive scans, we need to shift one more time and fill in the
       * bottom channel with identity.
       *
       * Some of this will get CSE'd with the first step but that's okay. The
       * code is cleaner this way.
       */
      nir_def *lower = nir_iand(b, mask, lt_mask);
      nir_def *has_buddy = nir_bany_inequal(b, lower, nir_imm_int(b, 0));
      nir_def *buddy = nir_ballot_find_msb(b, lower);

      nir_def *buddy_data = nir_shuffle(b, data, buddy);
      nir_def *identity = build_identity(b, data->bit_size, red_op);
      return nir_bcsel(b, has_buddy, buddy_data, identity);
   }

   case nir_intrinsic_inclusive_scan:
      return data;

   case nir_intrinsic_reduce: {
      /* For reductions, we need to take the top value of the scan */
      nir_def *idx = nir_ballot_find_msb(b, mask);
      return nir_shuffle(b, data, idx);
   }

   default:
      UNREACHABLE("Unsupported scan/reduce op");
   }
}
