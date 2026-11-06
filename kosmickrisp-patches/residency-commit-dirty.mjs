// SPDX-License-Identifier: MIT
// Commit the execution residency set only after its membership changes.
export const id = 'residency-commit-dirty';
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_device.h', source => source.replace(
    'struct kk_residency_set {\n   simple_mtx_t mutex;',
    'struct kk_residency_set {\n   simple_mtx_t mutex;\n   bool dirty; /* Protected by mutex, including commit. */'));
  edit('src/kosmickrisp/vulkan/kk_device.c', source => {
    const start = source.indexOf('void\nkk_device_add_bo_to_residency_set(');
    const end = source.indexOf('void\nkk_device_make_resources_resident(', start);
    if (start < 0 || end < 0) throw Error('Residency mutation anchors missing');
    const mutations = source.slice(start, end);
    const unlock = '   simple_mtx_unlock(&dev->residency_set.mutex);';
    if (mutations.split(unlock).length !== 7) throw Error('Residency mutation count changed');
    return source.replace(mutations, mutations.replaceAll(unlock,
      '   dev->residency_set.dirty = true;\n' + unlock)).replace(
      '   mtl_residency_set_commit(dev->residency_set.handle);',
      '   if (dev->residency_set.dirty) {\n      mtl_residency_set_commit(dev->residency_set.handle);\n      dev->residency_set.dirty = false;\n   }');
  });
  edit('src/kosmickrisp/vulkan/kk_sparse.c', source => source.replace(
    '      mtl_residency_set_remove_allocation(dev->residency_set.handle, resource);\n   simple_mtx_unlock(&dev->residency_set.mutex);',
    '      mtl_residency_set_remove_allocation(dev->residency_set.handle, resource);\n   dev->residency_set.dirty = true;\n   simple_mtx_unlock(&dev->residency_set.mutex);'));
  edit('src/kosmickrisp/vulkan/kk_device.h', source => source.replace(
    '   bool dirty; /* Protected by mutex, including commit. */',
    '   bool dirty; /* Protected by mutex, including commit. */\n' +
    '   bool requested; /* Persistent residency request, under the same mutex. */'));
  edit('src/kosmickrisp/vulkan/kk_device.c', source => source.replace(
    '   if (!dev->vk.enabled_features.pageableDeviceLocalMemory)\n      mtl_residency_set_request_residency(dev->residency_set.handle);',
    '   /* A residency request persists until endResidency. Subsequent commits\n' +
    '    * propagate membership changes to the already resident set. */\n' +
    '   if (!dev->vk.enabled_features.pageableDeviceLocalMemory &&\n' +
    '       !dev->residency_set.requested) {\n' +
    '      mtl_residency_set_request_residency(dev->residency_set.handle);\n' +
    '      dev->residency_set.requested = true;\n' +
    '   }'));
}
