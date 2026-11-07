// SPDX-License-Identifier: MIT
// Remove the TES subgroup failure expectation
export const id = "remove-the-tes-subgroup-failure-expectation";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/ci/kosmickrisp-fails.txt", source => source
    .replace("# macOS Apple M2 Max, Vulkan CTS 1.4.7.1.\n# Recorded failures are unresolved defects, not exclusions from execution.\n\n# Software binary64 graphics subgroup reduction: arm64 and x86_64.\ndEQP-VK.subgroups.arithmetic.graphics.subgroupadd_dvec3,Fail\n",
      () => "# macOS Apple M2 Max, Vulkan CTS 1.4.7.1.\n# Recorded failures are unresolved defects, not exclusions from execution.\n\n# Dedicated TES subgroup cases are NotSupported while TES is not advertised.\n# Other graphics stages and compute retain their ordinary CTS assertions.\n"));
}
