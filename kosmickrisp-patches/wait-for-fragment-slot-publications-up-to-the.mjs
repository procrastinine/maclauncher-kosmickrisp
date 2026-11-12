// SPDX-License-Identifier: MIT
// Wait for fragment slot publications up to the earlier limit
export const id = "wait-for-fragment-slot-publications-up-to-the";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_nir_lower_fsr.c", source => source
    .replace("#define FSR_SLOT_DATA 12\n#define FSR_SLOT_PROBES 16\n#define FSR_SLOT_AGE 8\n#define FSR_SLOT_TRIES (1u << 12)\n\nstatic unsigned\nfsr_output_words(nir_function_impl *impl)\n",
      () => "#define FSR_SLOT_DATA 12\n#define FSR_SLOT_PROBES 16\n#define FSR_SLOT_AGE 8\n#define FSR_SLOT_TRIES (1u << 18)\n\nstatic unsigned\nfsr_output_words(nir_function_impl *impl)\n"));
}
