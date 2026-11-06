// SPDX-License-Identifier: MIT
// A Metal residency set holds unique allocations. BO aliases keep that one
// registration until the final alias is destroyed; unique BOs stay O(1).
export const id = 'allocation-residency-references';
export function apply({edit}) {
  const change = (file, from, to) => edit('src/kosmickrisp/' + file, (_s, replace) => replace(from, to));
  change('vulkan/kk_bo.h', '#include "kosmickrisp/bridge/mtl_types.h"',
    '#include "kosmickrisp/bridge/mtl_types.h"\n#include "util/list.h"');
  change('vulkan/kk_bo.h', '   void *cpu;    // CPU address',
    '   void *cpu;    // CPU address\n   struct list_head residency_link;\n   struct list_head residency_alias_link;');
  change('vulkan/kk_device.h', '   mtl_residency_set *handle;',
    '   mtl_residency_set *handle;\n   struct list_head allocations;');
  change('vulkan/kk_device.h', 'void kk_device_add_heap_to_residency_set(struct kk_device *dev, mtl_heap *heap);',
    `void kk_device_add_bo_to_residency_set(struct kk_device *dev, struct kk_bo *bo);
void kk_device_remove_bo_from_residency_set(struct kk_device *dev, struct kk_bo *bo);
void kk_device_add_heap_to_residency_set(struct kk_device *dev, mtl_heap *heap);`);
  change('bridge/mtl_residency_set.h', '#include "mtl_types.h"', '#include "mtl_types.h"\n#include <stdbool.h>');
  change('bridge/mtl_residency_set.h', 'void mtl_residency_set_commit(mtl_residency_set *residency_set);',
    `bool mtl_residency_set_contains_allocation(mtl_residency_set *residency_set,
                                         mtl_allocation *allocation);
void mtl_residency_set_commit(mtl_residency_set *residency_set);`);
  change('bridge/mtl_residency_set.m', 'void\nmtl_residency_set_commit(',
    `bool
mtl_residency_set_contains_allocation(mtl_residency_set *residency_set,
                                      mtl_allocation *allocation)
{
   return [(id<MTLResidencySet>)residency_set
      containsAllocation:(id<MTLAllocation>)allocation];
}

void
mtl_residency_set_commit(`);
  change('vulkan/kk_device.c', '#include "kk_device.h"', '#include "kk_device.h"\n#include "kk_bo.h"');
  change('vulkan/kk_device.c', '   simple_mtx_init(&dev->residency_set.mutex, mtx_plain);',
    '   simple_mtx_init(&dev->residency_set.mutex, mtx_plain);\n   list_inithead(&dev->residency_set.allocations);');
  change('vulkan/kk_device.c', 'void\nkk_device_add_heap_to_residency_set(',
    `static mtl_allocation *
kk_bo_allocation(struct kk_bo *bo)
{
   return bo->mtl_handle ? (mtl_allocation *)bo->mtl_handle
                         : (mtl_allocation *)bo->map;
}

void
kk_device_add_bo_to_residency_set(struct kk_device *dev, struct kk_bo *bo)
{
   mtl_allocation *allocation = kk_bo_allocation(bo);
   simple_mtx_lock(&dev->residency_set.mutex);
   list_inithead(&bo->residency_alias_link);
   if (mtl_residency_set_contains_allocation(dev->residency_set.handle,
                                             allocation)) {
      /* Only imported native aliases need a lookup; ordinary new allocations
       * take the constant-time path and allocate no reference-count record. */
      list_for_each_entry(struct kk_bo, alias, &dev->residency_set.allocations,
                          residency_link) {
         if (kk_bo_allocation(alias) == allocation) {
            list_addtail(&bo->residency_alias_link, &alias->residency_alias_link);
            break;
         }
      }
   } else {
      mtl_residency_set_add_allocation(dev->residency_set.handle, allocation);
   }
   list_addtail(&bo->residency_link, &dev->residency_set.allocations);
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_remove_bo_from_residency_set(struct kk_device *dev, struct kk_bo *bo)
{
   simple_mtx_lock(&dev->residency_set.mutex);
   bool last_alias = list_is_empty(&bo->residency_alias_link);
   list_del(&bo->residency_alias_link);
   list_del(&bo->residency_link);
   if (last_alias)
      mtl_residency_set_remove_allocation(dev->residency_set.handle,
                                          kk_bo_allocation(bo));
   simple_mtx_unlock(&dev->residency_set.mutex);
}

void
kk_device_add_heap_to_residency_set(`);
  change('vulkan/kk_bo.c', '   kk_device_add_heap_to_residency_set(dev, handle);',
    '   kk_device_add_bo_to_residency_set(dev, bo);');
  change('vulkan/kk_bo.c',
    `   if (bo->mtl_handle)
      kk_device_remove_heap_from_residency_set(dev, bo->mtl_handle);
   else
      kk_device_remove_buffer_from_residency_set(dev, bo->map);`,
    '   kk_device_remove_bo_from_residency_set(dev, bo);');
  change('vulkan/kk_device_memory.c', '         kk_device_add_heap_to_residency_set(dev, mem->bo->mtl_handle);', '');
  change('vulkan/kk_device_memory.c', '         kk_device_add_buffer_to_residency_set(dev, mem->bo->map);', '');
  change('vulkan/kk_device_memory.c', '      mem->bo->gpu = mtl_buffer_get_gpu_address(mem->bo->map);',
    `      kk_device_add_bo_to_residency_set(dev, mem->bo);
      mem->bo->gpu = mtl_buffer_get_gpu_address(mem->bo->map);`);
}
