// SPDX-License-Identifier: MIT
/** Native vertex SIMD uses real active-lane data; Poly stages remain kernels. */
export const id = 'subgroup-graphics-stages';

export function apply({edit}) {
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => {
    function replace(from, to) {
      if (!s.includes(from)) throw new Error(`graphics subgroups: missing ${from.slice(0, 70)}`);
      s = s.replace(from, to);
    }
    // Metal permits vertex SIMD operations, but these two kernel/fragment
    // input attributes are not vertex-function inputs.
    replace(`   BITSET_FOREACH_SET(i, shader->info.system_values_read, SYSTEM_VALUE_MAX) {
      const char *sysval;`,
      `   BITSET_FOREACH_SET(i, shader->info.system_values_read, SYSTEM_VALUE_MAX) {
      if (shader->info.stage == MESA_SHADER_VERTEX &&
          (i == SYSTEM_VALUE_SUBGROUP_SIZE ||
           i == SYSTEM_VALUE_SUBGROUP_INVOCATION))
         continue;
      const char *sysval;`);
    // Prefix rank is not the hardware lane when the active mask has holes.
    // Select that rank's real mask bit once at entry, before shader divergence.
    // Other subgroup operations continue to use Metal's real physical lanes.
    replace(`   emit_local_vars(&ctx, shader);
   predeclare_ssa_values(&ctx, impl);`,
      `   emit_local_vars(&ctx, shader);
   if (shader->info.stage == MESA_SHADER_VERTEX) {
      if (BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_SUBGROUP_SIZE))
         P_IND(&ctx, "const uint gl_SubGroupSize = 32u;\\n");
      if (BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_SUBGROUP_INVOCATION)) {
         P_IND(&ctx, "uint kk_active_lanes = (uint)(ulong)simd_active_threads_mask();\\n");
         P_IND(&ctx, "uint kk_active_rank = simd_prefix_exclusive_sum(1u);\\n");
         P_IND(&ctx, "while (kk_active_rank != 0u) { kk_active_lanes &= kk_active_lanes - 1u; --kk_active_rank; }\\n");
         P_IND(&ctx, "const uint gl_SubGroupInvocation = ctz(kk_active_lanes);\\n");
      }
   }
   predeclare_ssa_values(&ctx, impl);`);
    return s;
  });

  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => {
    const from = '.subgroupSupportedStages =\n         VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,';
    if (!s.includes(from)) throw new Error('graphics subgroups: missing supported stage declaration');
    return s.replace(from, `.subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT |
                                 VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                 VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                                 VK_SHADER_STAGE_GEOMETRY_BIT,`);
  });
}
