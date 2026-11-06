// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';
export const id = 'fragment-interlock';
export const sourceFiles = [
  'kosmickrisp-patches/kosmickrisp-interlock-guard.h',
  'kosmickrisp-patches/kosmickrisp-interlock-device.h',
];
export function apply({edit, edits}) {
  const owned = name => fs.readFileSync(path.join(import.meta.dirname, name), 'utf8');
  edits.set('src/kosmickrisp/compiler/msl_interlock_guard.h', owned('kosmickrisp-interlock-guard.h'));
  edits.set('src/kosmickrisp/vulkan/kk_interlock_device.h', owned('kosmickrisp-interlock-device.h'));
  edit('src/kosmickrisp/compiler/nir_to_msl.c', s => s
    .replace('static void\nemit_inputs(', '#include "msl_interlock_guard.h"\n\nstatic void\nemit_inputs(')
    .replace('   P_IND(ctx, "constant Buffer &buf0 [[buffer(0)]],\\n");',
             '   msl_interlock_inputs(ctx);\n   P_IND(ctx, "constant Buffer &buf0 [[buffer(0)]],\\n");')
    .replace('intrinsic_to_msl(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *instr)\n{',
             'intrinsic_to_msl(struct nir_to_msl_ctx *ctx, nir_intrinsic_instr *instr)\n{\n   if (msl_interlock_intrinsic(ctx, instr))\n      return;')
    .replace('   const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];\n   if (instrinsic_needs_dest_type(instr)) {',
             '   bool guarded = msl_interlock_side_effect(ctx, instr);\n   if (guarded) {\n      P_IND(ctx, "if (kk_rog_live) {\\n");\n      ctx->indentlevel++;\n   }\n   const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];\n   if (instrinsic_needs_dest_type(instr)) {')
    .replace('      P_IND(ctx, "Unknown intrinsic %s\\n", info->name);\n   }\n}',
             '      P_IND(ctx, "Unknown intrinsic %s\\n", info->name);\n   }\n   if (guarded) {\n      ctx->indentlevel--;\n      P_IND(ctx, "}\\n");\n   }\n}')
    .replace('   emit_local_vars(&ctx, shader);',
             '   emit_local_vars(&ctx, shader);\n   msl_interlock_locals(&ctx);')
    .replace('   msl_gather_info(&ctx, options);',
             '   msl_interlock_prepare(shader);\n   msl_gather_info(&ctx, options);'));
  edit('src/kosmickrisp/vulkan/kk_device.h', s => s.replace(
    '   struct kk_bo *heap;', '   struct kk_bo *heap;\n   struct kk_bo *interlock_guard;'));
  edit('src/kosmickrisp/vulkan/kk_device.c', s => s
    .replace('#include "kk_shader.h"', '#include "kk_shader.h"\n#include "kk_interlock_device.h"')
    .replace('   result = kk_device_init_lib(dev);\n   if (result != VK_SUCCESS)\n      goto fail_sampler_heap;',
             '   result = kk_device_init_lib(dev);\n   if (result != VK_SUCCESS)\n      goto fail_sampler_heap;\n\n   result = kk_interlock_device_init(dev);\n   if (result != VK_SUCCESS)\n      goto fail_lib;')
    .replace('fail_sampler_heap:', 'fail_lib:\n   kk_device_finish_lib(dev);\nfail_sampler_heap:')
    .replace('   kk_device_finish_meta(dev);\n   kk_device_finish_lib(dev);',
             '   kk_device_finish_meta(dev);\n   kk_device_finish_lib(dev);\n   kk_interlock_device_finish(dev);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', s => s
    .replace('      mtl_set_address(cmd->argument_table, dev->samplers.table.bo->gpu, 1u);',
             '      mtl_set_address(cmd->argument_table, dev->samplers.table.bo->gpu, 1u);\n      if (dev->interlock_guard)\n         mtl_set_address(cmd->argument_table, dev->interlock_guard->gpu, KK_ARGUMENT_INTERLOCK_GUARD);'));
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', s => s.replace(
    '   kk_get_device_features(pdev, &supported_extensions, &supported_features);',
    '   kk_get_device_features(pdev, &supported_extensions, &supported_features);\n   if (pdev->info.gpu_apple_family >= 7) {\n      supported_extensions.EXT_fragment_shader_interlock = true;\n      supported_features.fragmentShaderSampleInterlock = true;\n   }'));
}
