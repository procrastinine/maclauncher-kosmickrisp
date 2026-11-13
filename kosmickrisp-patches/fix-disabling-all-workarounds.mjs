// SPDX-License-Identifier: MIT
// Fix disabling all workarounds
export const id = "fix-disabling-all-workarounds";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_physical_device.c", source => source
    .replace("   for (unsigned n; n = strcspn(list, \",\"), *list; list += MAX2(1, n)) {\n      if (n == all_len && !strncmp(list, all_workarounds, n)) {\n         settings->disabled_workarounds = UINT64_MAX;\n         break;\n      }\n\n      int index = atoi(list);\n",
      () => "   for (unsigned n; n = strcspn(list, \",\"), *list; list += MAX2(1, n)) {\n      if (n == all_len && !strncmp(list, all_workarounds, n)) {\n         settings->disabled_workarounds = UINT64_MAX;\n         return;\n      }\n\n      int index = atoi(list);\n"));
}
