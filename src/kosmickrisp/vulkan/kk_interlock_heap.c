/* SPDX-License-Identifier: MIT */
#include "kk_interlock_heap.h"
#include "kk_device.h"
#include "kk_cmd_buffer.h"
#include "kk_bo.h"
#include "poly/geometry.h"
#include "util/u_atomic.h"
#include "vk_alloc.h"

struct kk_interlock_heap {
   struct kk_device *device;
   struct kk_bo *bo;
   uint32_t references;
};

VkResult
kk_interlock_heap_select(struct kk_cmd_buffer *cmd, bool selected)
{
   cmd->interlock_heap_active = false;
   if (!selected)
      return VK_SUCCESS;
   if (!cmd->interlock_heap) {
      struct kk_device *dev = kk_cmd_buffer_device(cmd);
      struct kk_interlock_heap *heap = vk_zalloc(&dev->vk.alloc,
         sizeof(*heap), 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!heap)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      /* Match Poly's existing finite capture budget. Reuse it after each
       * selected draw's private completion gate, rather than accumulating a
       * primitive-by-framebuffer table or allocating per replay pass. */
      const uint64_t size = KK_POLY_HEAP_SIZE;
      VkResult result = kk_alloc_bo(dev, &cmd->vk.base, size, 0, &heap->bo);
      if (result != VK_SUCCESS) {
         vk_free(&dev->vk.alloc, heap);
         return result;
      }
      heap->device = dev;
      heap->references = 1;
      *(struct poly_heap *)heap->bo->cpu = (struct poly_heap){
         .base = heap->bo->gpu + sizeof(struct poly_heap),
         .size = size - sizeof(struct poly_heap),
      };
      cmd->interlock_heap = heap;
   }
   cmd->interlock_heap_active = true;
   cmd->interlock_heap_used = false;
   return VK_SUCCESS;
}

struct kk_bo *
kk_interlock_heap_bo(struct kk_interlock_heap *heap)
{
   return heap->bo;
}

struct kk_interlock_heap *
kk_interlock_heap_ref(struct kk_interlock_heap *heap)
{
   if (heap)
      p_atomic_inc(&heap->references);
   return heap;
}

void
kk_interlock_heap_unref(struct kk_interlock_heap *heap)
{
   if (!heap || !p_atomic_dec_zero(&heap->references))
      return;
   struct kk_device *dev = heap->device;
   kk_destroy_bo(dev, heap->bo);
   vk_free(&dev->vk.alloc, heap);
}

void
kk_interlock_heap_finish(struct kk_cmd_buffer *cmd)
{
   kk_interlock_heap_unref(cmd->interlock_heap);
   cmd->interlock_heap = NULL;
   cmd->interlock_heap_active = false;
   cmd->interlock_heap_used = false;
}
