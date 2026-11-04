/* SPDX-License-Identifier: MIT */
#ifndef KK_ATOMIC_INT64_USAGE_H
#define KK_ATOMIC_INT64_USAGE_H
#include "nir.h"

static inline bool
kk_is_atomic64(nir_intrinsic_instr *intr)
{
   bool store = false;
   switch (intr->intrinsic) {
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      return intr->def.bit_size == 64;
   case nir_intrinsic_store_global:
   case nir_intrinsic_store_shared: store = true; FALLTHROUGH;
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_shared:
      return (store ? intr->src[0].ssa->bit_size : intr->def.bit_size) == 64 &&
             (nir_intrinsic_access(intr) & ACCESS_ATOMIC);
   default: return false;
   }
}

static inline bool
kk_is_image64(nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_bindless_image_load:
   case nir_intrinsic_bindless_image_atomic:
   case nir_intrinsic_bindless_image_atomic_swap:
      return intr->def.bit_size == 64;
   case nir_intrinsic_bindless_image_store:
      return intr->src[3].ssa->bit_size == 64;
   default: return false;
   }
}

static inline bool
kk_is_image_atomic64(nir_intrinsic_instr *intr)
{
   return kk_is_image64(intr) &&
      (nir_intrinsic_has_atomic_op(intr) ||
       (nir_intrinsic_access(intr) & ACCESS_ATOMIC));
}

#endif
