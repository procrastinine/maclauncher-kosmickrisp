// SPDX-License-Identifier: MIT
export function apply({edit}) {
// This one minimum feeds both the Vulkan limit and SPIR-V pointer alignment.
// Descriptors keep the exact byte address/range; robustness2's separate
// four-byte bounds granularity stays unchanged instead of expanding the view.
edit('src/kosmickrisp/vulkan/kk_private.h', s => s.replace(
 '#define KK_MIN_SSBO_ALIGNMENT            16',
 '#define KK_MIN_SSBO_ALIGNMENT            4'));

// MSL packed vectors relax vector alignment to scalar alignment. A 64-bit
// scalar can still have only four-byte pointer alignment, so use Mesa's shared
// memory-access lowering to split that access without rounding its address.
// Existing naturally aligned accesses retain their bit size/component count.
edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
 .replace('bool\nmsl_optimize_nir(struct nir_shader *nir)',
 `static nir_mem_access_size_align
kk_mem_access_size_align(UNUSED nir_intrinsic_op intrin, uint8_t bytes,
                         uint8_t bit_size, uint32_t align_mul,
                         uint32_t align_offset, UNUSED bool offset_is_const,
                         enum gl_access_qualifier access,
                         UNUSED const void *cb_data)
{
   const unsigned align = nir_combined_align(align_mul, align_offset);
   /* Atomic loads/stores retain the full-width atomic implementation. */
   if (!(access & ACCESS_ATOMIC))
      bit_size = MIN2(bit_size / 8, align) * 8;

   return (nir_mem_access_size_align){
      .num_components = MIN2(bytes / (bit_size / 8), 4),
      .bit_size = bit_size,
      .align = MIN2(bit_size / 8, align),
      .shift = nir_mem_access_shift_method_scalar,
   };
}

bool
msl_optimize_nir(struct nir_shader *nir)`)
 .replace('   NIR_PASS(_, nir, nir_lower_int64);',
 `   const nir_lower_mem_access_bit_sizes_options memory_options = {
      .callback = kk_mem_access_size_align,
      .modes = nir_var_mem_global | nir_var_mem_constant,
   };
   NIR_PASS(_, nir, nir_lower_mem_access_bit_sizes, &memory_options);
   NIR_PASS(_, nir, nir_lower_int64);`));
}
