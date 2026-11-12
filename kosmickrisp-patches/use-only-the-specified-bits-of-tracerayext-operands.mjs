// SPDX-License-Identifier: MIT
// Use only the specified bits of traceRayEXT operands
export const id = "use-only-the-specified-bits-of-tracerayext-operands";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/vulkan/kk_rt_cps.c", source => source
    .replace("         offsetof(struct kk_rt_cps_context, tmax),\n         offsetof(struct kk_rt_cps_context, payload),\n      };\n      for (unsigned i = 0; i < ARRAY_SIZE(fields); ++i)\n         context_store(b, fields[i], (i == 0 || i == 10) ?\n            nir_u2u64(b, in->src[i].ssa) : in->src[i].ssa);\n      break;\n   }\n   case nir_intrinsic_rt_execute_callable:\n",
      () => "         offsetof(struct kk_rt_cps_context, tmax),\n         offsetof(struct kk_rt_cps_context, payload),\n      };\n      /* Only the low 8 cull-mask bits, 4 SBT offset and stride bits and 16\n       * miss-index bits of traceRayEXT operands are used. */\n      static const uint32_t used_bits[ARRAY_SIZE(fields)] = {\n         [2] = 0xff, [3] = 0xf, [4] = 0xf, [5] = 0xffff,\n      };\n      for (unsigned i = 0; i < ARRAY_SIZE(fields); ++i) {\n         nir_def *value = in->src[i].ssa;\n         if (used_bits[i])\n            value = nir_iand_imm(b, value, used_bits[i]);\n         context_store(b, fields[i], (i == 0 || i == 10) ? nir_u2u64(b, value) : value);\n      }\n      break;\n   }\n   case nir_intrinsic_rt_execute_callable:\n"));
}
