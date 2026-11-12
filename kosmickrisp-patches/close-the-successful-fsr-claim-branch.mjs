// SPDX-License-Identifier: MIT
// Close the successful FSR claim branch
export const id = "close-the-successful-fsr-claim-branch";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_nir_lower_fsr.c", source => source
    .replace("               nir_store_var(b, slot, addr, 1);\n               nir_jump(b, nir_jump_break);\n            }\n            /* Another lane of this fragment claimed it first. */\n            nir_push_if(b, nir_ieq(b, seen, ctx->tag));\n            {\n",
      () => "               nir_store_var(b, slot, addr, 1);\n               nir_jump(b, nir_jump_break);\n            }\n            nir_pop_if(b, NULL);\n            /* Another lane of this fragment claimed it first. */\n            nir_push_if(b, nir_ieq(b, seen, ctx->tag));\n            {\n")
    .replace("               nir_jump(b, nir_jump_break);\n            }\n            nir_pop_if(b, NULL);\n            nir_pop_if(b, NULL);\n            /* Lost the race to another fragment: examine the same slot again. */\n         }\n         nir_push_else(b, NULL);\n",
      () => "               nir_jump(b, nir_jump_break);\n            }\n            nir_pop_if(b, NULL);\n            /* Lost the race to another fragment: examine the same slot again. */\n         }\n         nir_push_else(b, NULL);\n"));
}
