/* SPDX-License-Identifier: MIT */
#ifndef KK_INTERLOCK_QUAD_NIR_H
#define KK_INTERLOCK_QUAD_NIR_H

#include "compiler/nir/nir_builder.h"

/* Input values are captured by native multisample rasterization. A covered
 * lane writes its own original quad record, including helper values. The
 * single-sample transaction can then reproduce derivatives without deriving
 * a second interpolation plane after clipping or shifting geometry. */
struct kk_interlock_quad_layout {
   uint16_t slot_offset[NUM_TOTAL_VARYING_SLOTS];
   uint16_t stride;
};

static void
kk_interlock_quad_layout_init(struct kk_interlock_quad_layout *layout,
                               uint64_t inputs,
                               const nir_alu_type *types,
                               const uint8_t *components)
{
   memset(layout, 0, sizeof(*layout));
   u_foreach_bit64(slot, inputs) {
      layout->slot_offset[slot] = layout->stride;
      unsigned bits = MAX2(nir_alu_type_get_type_size(types[slot]), 32);
      layout->stride += ALIGN_POT(components[slot] * (bits / 8), 16);
   }
}

static void
kk_interlock_quad_store_if(nir_builder *b, nir_def *quad_address,
                            unsigned lane_stride, nir_def *value,
                            nir_def *store)
{
   nir_def *lanes[4];
   for (unsigned i = 0; i < 4; ++i)
      lanes[i] = nir_quad_broadcast(b, value, nir_imm_int(b, i));
   nir_push_if(b, nir_iand(b, store,
                           nir_inot(b, nir_is_helper_invocation(b, 1))));
   for (unsigned i = 0; i < 4; ++i)
      nir_store_global(b, lanes[i],
         nir_iadd_imm(b, quad_address, i * lane_stride), .align_mul = 4);
   nir_pop_if(b, NULL);
}

static void
kk_interlock_quad_store(nir_builder *b, nir_def *quad_address,
                         unsigned lane_stride, nir_def *value)
{
   kk_interlock_quad_store_if(b, quad_address, lane_stride, value,
                                nir_imm_true(b));
}

static nir_def *
kk_interlock_quad_load(nir_builder *b, nir_def *quad_address,
                        unsigned lane_stride, unsigned components,
                        unsigned bits, nir_def *pixel)
{
   nir_def *x = nir_iand_imm(b, nir_channel(b, pixel, 0), 1);
   nir_def *y = nir_iand_imm(b, nir_channel(b, pixel, 1), 1);
   nir_def *lane = nir_iadd(b, x, nir_ishl_imm(b, y, 1));
   nir_def *address = nir_iadd(b, quad_address,
      nir_u2u64(b, nir_imul_imm(b, lane, lane_stride)));
   return nir_load_global_constant(b, components, bits, address,
                                    .align_mul = 4);
}

#endif
