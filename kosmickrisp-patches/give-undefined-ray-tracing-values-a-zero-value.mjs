// SPDX-License-Identifier: MIT
// Give undefined ray tracing values a zero value
export const id = "give-undefined-ray-tracing-values-a-zero-value";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("   struct kk_physical_device *pdev = kk_device_physical(dev);\n   KK_PERF_VOID(dev, KK_PERF_NIR,\n                kk_finish_msl_nir(nir, opts && opts->pipeline_statistics));\n   if (has_framebuffer_reads)\n      *has_framebuffer_reads = nir->info.stage == MESA_SHADER_FRAGMENT &&\n                               nir->info.outputs_read != 0;\n",
      () => "   struct kk_physical_device *pdev = kk_device_physical(dev);\n   KK_PERF_VOID(dev, KK_PERF_NIR,\n                kk_finish_msl_nir(nir, opts && opts->pipeline_statistics));\n   /* Ray tracing parts can keep undefined values, including 64-bit addresses\n    * on paths that pipeline flags exclude. MSL has no undefined literal of\n    * every width, so they become zero. */\n   if (mesa_shader_stage_is_rt(nir->info.stage))\n      NIR_PASS(_, nir, nir_lower_undef_to_zero, NULL);\n   if (has_framebuffer_reads)\n      *has_framebuffer_reads = nir->info.stage == MESA_SHADER_FRAGMENT &&\n                               nir->info.outputs_read != 0;\n"));
}
