// SPDX-License-Identifier: MIT
// Prepare sparse guards for descriptor-buffer shaders
export const id = "prepare-sparse-guards-for-descriptor-buffer-shaders";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_sparse_pipeline.h", source => source
    .replace("      }\n      modes |= stage_modes;\n      *always_modes |= physical_modes;\n      if (infos[i].flags & VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT)\n         *always_modes |= stage_modes;\n   }\n   /* The lazy native object represents a linked VS/CS pipeline. Standalone\n",
      () => "      }\n      modes |= stage_modes;\n      *always_modes |= physical_modes;\n      if (infos[i].flags & (VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT |\n                            VK_SHADER_CREATE_DESCRIPTOR_BUFFER_BIT_MESA))\n         *always_modes |= stage_modes;\n   }\n   /* The lazy native object represents a linked VS/CS pipeline. Standalone\n"));
}
