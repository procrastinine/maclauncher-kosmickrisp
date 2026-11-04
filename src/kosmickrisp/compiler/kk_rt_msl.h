/* SPDX-License-Identifier: MIT */
#ifndef KK_RT_MSL_H
#define KK_RT_MSL_H
#include "kosmickrisp/vulkan/kk_rt_abi.h"

static void
kk_rt_emit_function(struct nir_to_msl_ctx *ctx, nir_function_impl *impl)
{
   P(ctx, "[[visible]] void %s(device uchar *kk_rt_context)\n{\n",
      impl->function->name);
   ctx->indentlevel++;
   /* These are the same application root and sampler table as ordinary
    * shaders. Context is a function argument, not a new Metal buffer slot. */
   P_IND(ctx, "constant Buffer &buf0 = *(constant Buffer*)"
              "(*(device ulong*)(kk_rt_context + %zu));\n",
      offsetof(struct kk_rt_cps_context, root));
   P_IND(ctx, "constant SamplerTable &sampler_table = *(constant SamplerTable*)"
              "(*(device ulong*)(kk_rt_context + %zu));\n",
      offsetof(struct kk_rt_cps_context, sampler_table));
   P_IND(ctx, "uint gl_SubGroupInvocation = *(device uint*)(kk_rt_context + %zu);\n",
      offsetof(struct kk_rt_cps_context, lane));
   P_IND(ctx, "uint gl_SubGroupSize = *(device uint*)(kk_rt_context + %zu);\n",
      offsetof(struct kk_rt_cps_context, subgroup_size));
   predeclare_ssa_values(ctx, impl);
   foreach_list_typed(nir_cf_node, node, node, &impl->body)
      cf_node_to_metal(ctx, node);
   ctx->indentlevel--;
   P(ctx, "}\n");
}
#endif
