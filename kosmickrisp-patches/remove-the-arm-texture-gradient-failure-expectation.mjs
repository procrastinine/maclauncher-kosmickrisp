// SPDX-License-Identifier: MIT
// Remove the ARM texture-gradient failure expectation
export const id = "remove-the-arm-texture-gradient-failure-expectation";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/ci/kosmickrisp-fails.txt", source => source
    .replace("\n# Software binary64 graphics subgroup reduction: arm64 and x86_64.\ndEQP-VK.subgroups.arithmetic.graphics.subgroupadd_dvec3,Fail\n\n# Explicit texture gradients and mipmap selection: observed on arm64.\n# The corresponding x86_64 case has not been executed on the affected source.\ndEQP-VK.texture.explicit_lod.2d.derivatives.linear_linear_mipmap_linear,Fail\n",
      () => "\n# Software binary64 graphics subgroup reduction: arm64 and x86_64.\ndEQP-VK.subgroups.arithmetic.graphics.subgroupadd_dvec3,Fail\n"));
}
