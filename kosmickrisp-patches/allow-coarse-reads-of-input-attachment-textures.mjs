// SPDX-License-Identifier: MIT
// Allow coarse reads of input attachment textures
export const id = "allow-coarse-reads-of-input-attachment-textures";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_nir_lower_fsr.c", source => source
    .replace("bool\nkk_nir_fsr_supported(nir_shader *nir, bool force_fine)\n{\n   if (nir->info.fs.uses_sample_shading || nir->info.fs.uses_fbfetch_output ||\n       (!force_fine &&\n        (nir->info.outputs_written & (BITFIELD64_BIT(FRAG_RESULT_DEPTH) |\n                                      BITFIELD64_BIT(FRAG_RESULT_STENCIL) |\n",
      () => "bool\nkk_nir_fsr_supported(nir_shader *nir, bool force_fine)\n{\n   /* Input attachments already use texture reads at FragCoord here. Their\n    * uses_fbfetch_output metadata does not require native framebuffer fetch;\n    * actual output reads are rejected by the intrinsic scan below. */\n   if (nir->info.fs.uses_sample_shading ||\n       (!force_fine &&\n        (nir->info.outputs_written & (BITFIELD64_BIT(FRAG_RESULT_DEPTH) |\n                                      BITFIELD64_BIT(FRAG_RESULT_STENCIL) |\n"));
}
