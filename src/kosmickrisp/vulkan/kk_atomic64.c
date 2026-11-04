/* SPDX-License-Identifier: MIT */
#include "kk_atomic64.h"
#include "kk_device.h"
#include "kk_bo.h"
#include "kk_shader.h"
#include "vk_alloc.h"
#include "util/u_atomic.h"
#include "kosmickrisp/bridge/mtl_bridge.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "kosmickrisp/libkk/kk_argument_table.h"

struct kk_atomic64_state {
   simple_mtx_t mutex;
   struct kk_bo *bo;
   bool ready; /* bo is allocated */
};

static VkResult
kk_atomic64_address(struct kk_device *dev, uint64_t *address)
{
   struct kk_atomic64_state *state = p_atomic_read(&dev->atomic64_locks);
   if (!state) {
      struct kk_atomic64_state *candidate = vk_zalloc(&dev->vk.alloc,
         sizeof(*candidate), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!candidate)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      simple_mtx_init(&candidate->mutex, mtx_plain);
      state = p_atomic_cmpxchg(&dev->atomic64_locks,
         (struct kk_atomic64_state *)NULL, candidate);
      if (state) {
         simple_mtx_destroy(&candidate->mutex);
         vk_free(&dev->vk.alloc, candidate);
      } else {
         state = candidate;
      }
   }

   /* Publish only the small CPU owner with CAS. Its mutex prevents concurrent
    * pipeline workers from allocating multiple native lock buffers. */
   simple_mtx_lock(&state->mutex);
   VkResult result = VK_SUCCESS;
   if (!state->bo) {
      result = kk_alloc_bo(dev, &dev->vk.base, 65536 * sizeof(uint32_t), 64,
                           &state->bo);
      if (result == VK_SUCCESS)
         memset(state->bo->cpu, 0, 65536 * sizeof(uint32_t));
   }
   if (result == VK_SUCCESS)
      *address = state->bo->gpu;
   simple_mtx_unlock(&state->mutex);
   return result;
}

/* Lazily translated compute shaders build natively on compiler workers,
 * which must not call the device allocator. Their creation on an API thread
 * allocates the lock table once; later calls read one flag. */
VkResult
kk_atomic64_prepare(struct kk_device *dev)
{
   struct kk_atomic64_state *state = p_atomic_read(&dev->atomic64_locks);
   if (state && p_atomic_read(&state->ready))
      return VK_SUCCESS;
   uint64_t address;
   VkResult result = kk_atomic64_address(dev, &address);
   if (result == VK_SUCCESS)
      p_atomic_set(&dev->atomic64_locks->ready, true);
   return result;
}

void
kk_atomic64_finish_device(struct kk_device *dev)
{
   struct kk_atomic64_state *state = dev->atomic64_locks;
   if (!state)
      return;
   if (state->bo)
      kk_destroy_bo(dev, state->bo);
   simple_mtx_destroy(&state->mutex);
   vk_free(&dev->vk.alloc, state);
   dev->atomic64_locks = NULL;
}

VkResult
kk_msl_function_sampler_base(struct kk_device *dev, const struct msl_compile_data *data,
                mtl_library *library, const char *entry,
                mtl_function_descriptor **out)
{
   *out = mtl_new_library_function_descriptor(library, entry);
   if (!*out)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!data->uses_atomic64_locks)
      return VK_SUCCESS;

   uint64_t address;
   VkResult result = kk_atomic64_address(dev, &address);
   if (result == VK_SUCCESS) {
      mtl_function_descriptor *specialized = mtl_specialize_function_uint64(
         *out, KK_FUNCTION_CONSTANT_ATOMIC64_LOCK_TABLE, address);
      if (specialized) {
         mtl_release(*out);
         *out = specialized;
         return VK_SUCCESS;
      }
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   mtl_release(*out);
   *out = NULL;
   return result;
}

/* Direct consumers without a late-upgrade cache must retain emulation. */
VkResult
kk_msl_function(struct kk_device *dev, const struct msl_compile_data *data,
   mtl_library *library, const char *entry, mtl_function_descriptor **out)
{
   VkResult result = kk_msl_function_sampler_base(dev, data, library, entry, out);
   if (result != VK_SUCCESS || !data->uses_sampler_emulation) return result;
   mtl_function_descriptor *function = mtl_specialize_sampler_emulation(*out, KK_SAMPLER_EMULATION_ALL);
   mtl_release(*out);
   *out = function;
   return function ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
