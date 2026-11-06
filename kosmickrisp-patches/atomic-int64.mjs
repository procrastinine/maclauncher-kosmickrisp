// SPDX-License-Identifier: MIT
import fs from 'node:fs';

/** Serialize full-width integer atomics with a device-owned hashed lock table. */
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-atomic-int64.h', 'kosmickrisp-patches/kosmickrisp-atomic-int64.metal'];
export function apply({edit: editSource, edits}) {
  const edit = (name, transform) => editSource('src/kosmickrisp/' + name, transform);
  edit('vulkan/kk_physical_device.c', s => s
    .replace('.KHR_shader_atomic_int64 = false,', '.KHR_shader_atomic_int64 = true,')
    .replace('      .shaderInt64 = true,', `      .shaderInt64 = true,
      .shaderBufferInt64Atomics = true,
      .shaderSharedInt64Atomics = true,`));
  edit('vulkan/kk_device.h', s => s.replace('   struct kk_bo *heap;',
    '   struct kk_bo *atomic64_locks;\n   struct kk_bo *heap;'));
  edit('vulkan/kk_device.c', s => s
    .replace('   if (pdev->settings.gpu_capture_enabled) {', `   if (dev->vk.enabled_features.shaderBufferInt64Atomics ||
       dev->vk.enabled_features.shaderSharedInt64Atomics) {
      result = kk_alloc_bo(dev, &dev->vk.base, 65536 * sizeof(uint32_t), 64,
                           &dev->atomic64_locks);
      if (result != VK_SUCCESS) {
         kk_device_finish_lib(dev);
         goto fail_sampler_heap;
      }
      memset(dev->atomic64_locks->cpu, 0, 65536 * sizeof(uint32_t));
   }

   if (pdev->settings.gpu_capture_enabled) {`)
    .replace('   /* Geometry heap */', `   if (dev->atomic64_locks)
      kk_destroy_bo(dev, dev->atomic64_locks);

   /* Geometry heap */`));
  edit('vulkan/kk_cmd_buffer.c', (s, replace) => replace(
    '   memcpy(root_ptr.cpu, root, size);',
    '   memcpy(root_ptr.cpu, root, size);\n   struct kk_device *dev = kk_cmd_buffer_device(cmd);\n   if (dev->atomic64_locks)\n      ((struct kk_root_descriptor_table *)root_ptr.cpu)->addr = dev->atomic64_locks->gpu;'));
  edit('compiler/nir_to_msl.c', s => s
    .replace('static void\nintrinsic_to_msl(', '#include "kk_atomic_int64.h"\n\nstatic void\nintrinsic_to_msl(')
    .replace('   const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];\n   if (instrinsic_needs_dest_type(instr)) {',
      '   if (kk_emit_atomic64(ctx, instr)) return;\n   const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];\n   if (instrinsic_needs_dest_type(instr)) {')
    .replace('   P(&ctx, "using namespace metal;\\n");',
      '   P(&ctx, "using namespace metal;\\n");\n   if (kk_uses_atomic64(shader)) P(&ctx, "%s", kk_atomic64_msl);'));
  edits.set('src/kosmickrisp/compiler/kk_atomic_int64.h',
    fs.readFileSync(new URL('./kosmickrisp-atomic-int64.h', import.meta.url), 'utf8'));
  const msl = fs.readFileSync(new URL('./kosmickrisp-atomic-int64.metal', import.meta.url), 'utf8');
  edits.set('src/kosmickrisp/compiler/kk_atomic_int64_msl.h',
    'static const char kk_atomic64_msl[] = ' + JSON.stringify(msl) + ';\n');
}
