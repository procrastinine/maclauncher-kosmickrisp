// SPDX-License-Identifier: MIT
// Add an isolated triangle-merge diagnostic toggle
export const id = "add-an-isolated-triangle-merge-diagnostic-toggle";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("   _mesa_blake3_update(&blake3_ctx, instance->driver_build_sha,\n                       sizeof(instance->driver_build_sha));\n\n   unsigned char blake3[BLAKE3_KEY_LEN];\n   _mesa_blake3_final(&blake3_ctx, blake3);\n\n",
      () => "   _mesa_blake3_update(&blake3_ctx, instance->driver_build_sha,\n                       sizeof(instance->driver_build_sha));\n\n   /* Keep diagnostic shaders out of default application pipeline caches and\n    * shader binaries. Disk/native caches also key the workaround mask. */\n   if (pdev->settings.disabled_workarounds & BITFIELD64_BIT(19)) {\n      static const char diagnostic[] = \"triangle-merge-disabled\";\n      _mesa_blake3_update(&blake3_ctx, diagnostic, sizeof(diagnostic));\n   }\n\n   unsigned char blake3[BLAKE3_KEY_LEN];\n   _mesa_blake3_final(&blake3_ctx, blake3);\n\n"));
  edit("src/kosmickrisp/vulkan/kk_shader.c", source => source
    .replace("                            (pdev->info.gpu_apple_family == 7 ||\n                             pdev->info.gpu_apple_family == 8) &&\n                            state->ms && state->ms->rasterization_samples > 1;\n      if (!ms_bug_present)\n         NIR_PASS(_, nir, msl_disable_triangle_merge);\n   }\n\n",
      () => "                            (pdev->info.gpu_apple_family == 7 ||\n                             pdev->info.gpu_apple_family == 8) &&\n                            state->ms && state->ms->rasterization_samples > 1;\n      /* KK_WORKAROUND_19: diagnostic-only triangle-merge comparison. */\n      if (!ms_bug_present &&\n          !(pdev->settings.disabled_workarounds & BITFIELD64_BIT(19)))\n         NIR_PASS(_, nir, msl_disable_triangle_merge);\n   }\n\n"));
}
