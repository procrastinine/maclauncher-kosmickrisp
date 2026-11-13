// SPDX-License-Identifier: MIT
// Disable KK_WORKAROUND_10 on M3+
export const id = "disable-kk-workaround-10-on-m3";
export function apply({edit, edits}) {
  edit("docs/drivers/kosmickrisp/workarounds.rst", source => source
    .replace("\n| Log:\n| 2026-05-14: Workaround implemented\n\nKK_WORKAROUND_9\n---------------\n",
      () => "\n| Log:\n| 2026-05-14: Workaround implemented\n| 2026-11-05: Workaround disabled on M3+\n\nKK_WORKAROUND_9\n---------------\n"));
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("   if (pdev->info.gpu_apple_family < 9) {\n      settings->disabled_workarounds |= BITFIELD64_BIT(18);\n   }\n}\n\nVkResult\n",
      () => "   if (pdev->info.gpu_apple_family < 9) {\n      settings->disabled_workarounds |= BITFIELD64_BIT(18);\n   }\n   if (pdev->info.gpu_apple_family >= 9) {\n      settings->disabled_workarounds |= BITFIELD64_BIT(10);\n   }\n}\n\nVkResult\n"));
}
