// SPDX-License-Identifier: MIT
// Align index buffers to 4 bytes on macOS 26
export const id = "align-index-buffers-to-4-bytes-on-macos";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_cmd_draw.c", source => source
    .replace("      range.range = sizeof(uint32_t);\n   }\n\n   return range;\n}\n\n",
      () => "      range.range = sizeof(uint32_t);\n   }\n\n   /* KK_WORKAROUND_12: macOS 26 requires addr + range to be 4 byte aligned due\n    * to a known bug where Metal drops the last index if addr + range is not 4\n    * byte aligned. The extra bytes will not be read. The issue is not present\n    * in macOS 27.\n    */\n   struct kk_physical_device *pdev =\n      kk_device_physical(kk_cmd_buffer_device(cmd));\n   if (!(pdev->settings.disabled_workarounds & BITFIELD64_BIT(12)))\n      range.range = align64(range.addr + range.range, 4u) - range.addr;\n\n   return range;\n}\n\n"));
}
