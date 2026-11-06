// SPDX-License-Identifier: MIT
export const id = 'zero-initialize-allocation-clear';

/** Establish zero contents before publishing non-imported device memory. */
export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_device_memory.c', (s) => s
    .replace('#include <inttypes.h>', '#include <inttypes.h>\n#include <string.h>')
    .replace(`      if (result != VK_SUCCESS)
         goto fail_alloc;
   }

   *pMem = kk_device_memory_to_handle(mem);`, `      if (result != VK_SUCCESS)
         goto fail_alloc;

      /* Imports never enter this branch. VUID-flags-10760 forbids the zero
       * allocation flag on an import; supplied contents must be preserved.
       */
      if (mem->vk.alloc_flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT) {
         if (mem->bo->cpu) {
            memset(mem->bo->cpu, 0, mem->bo->size_B);
         } else {
            enum mtl_command_queue_error error =
               mtl_buffer_zero_sync(dev->mtl_handle, mem->bo->map);
            if (error != MTL_COMMAND_QUEUE_ERROR_NONE) {
               kk_destroy_bo(dev, mem->bo);
               if (error == MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY)
                  result = vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
               else
                  result = vk_device_set_lost(&dev->vk,
                                             "Device memory zero fill failed");
               goto fail_alloc;
            }
         }
      }
   }

   *pMem = kk_device_memory_to_handle(mem);`));
  edit('src/kosmickrisp/bridge/mtl_buffer.h', s => s.replace(
    '/* Allocation from buffer */',
    `/* Zero the complete buffer and wait before exposing the allocation. */
enum mtl_command_queue_error
mtl_buffer_zero_sync(mtl_device *device, mtl_buffer *buffer);

/* Allocation from buffer */`));
  edit('src/kosmickrisp/bridge/mtl_buffer.m', s => s
    .replace('#include <Metal/MTLBuffer.h>', '#include <Metal/Metal.h>')
    .replace('uint64_t\nmtl_buffer_get_length', `/* A private allocation is not visible to any Vulkan queue until this
 * independent fill has completed. No queue state or pending BO clear is kept.
 * The temporary queue exists only for allocations requesting zero contents.
 */
enum mtl_command_queue_error
mtl_buffer_zero_sync(mtl_device *device, mtl_buffer *buffer)
{
   @autoreleasepool {
      id<MTLCommandQueue> queue = [(id<MTLDevice>)device newCommandQueue];
      if (!queue)
         return MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY;

      id<MTLCommandBuffer> commands = [queue commandBuffer];
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      if (!blit) {
         [queue release];
         return MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY;
      }
      id<MTLBuffer> target = (id<MTLBuffer>)buffer;
      [blit fillBuffer:target range:NSMakeRange(0, target.length) value:0];
      [blit endEncoding];
      [commands commit];
      [commands waitUntilCompleted];

      enum mtl_command_queue_error result = MTL_COMMAND_QUEUE_ERROR_NONE;
      if (commands.status != MTLCommandBufferStatusCompleted)
         result = commands.error.code == MTLCommandBufferErrorOutOfMemory
                     ? MTL_COMMAND_QUEUE_ERROR_OUT_OF_MEMORY
                     : MTL_COMMAND_QUEUE_ERROR_INTERNAL;
      [queue release];
      return result;
   }
}

uint64_t
mtl_buffer_get_length`));
}
