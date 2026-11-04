/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_heap.h"
#include "mtl_sparse.h"

/* TODO_KOSMICKRISP Remove */
#include "kk_private.h"
#include "kk_image_layout.h"

#include <Metal/MTLHeap.h>
#import <Foundation/Foundation.h>
#include <objc/runtime.h>

/* Creation */
mtl_heap *
mtl_new_heap(mtl_device *device, uint64_t size,
             enum mtl_resource_options resource_options)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;
      MTLHeapDescriptor *descriptor = [[MTLHeapDescriptor new] autorelease];
      descriptor.type = MTLHeapTypePlacement;
      descriptor.resourceOptions = (MTLResourceOptions)resource_options;
      descriptor.size = size;
      descriptor.sparsePageSize = MTLSparsePageSize16;
      if (descriptor.storageMode == MTLStorageModePrivate &&
          size >= 65536 && size % 65536 == 0 &&
          mtl_device_supports_placement_sparse(device))
         descriptor.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
      return [dev newHeapWithDescriptor:descriptor];
   }
}

/* Utils */
uint64_t
mtl_heap_get_size(mtl_heap *heap)
{
   @autoreleasepool {
      id<MTLHeap> hp = (id<MTLHeap>)heap;
      return hp.size;
   }
}

enum mtl_storage_mode
mtl_heap_get_storage_mode(mtl_heap *heap)
{
   return (enum mtl_storage_mode)((id<MTLHeap>)heap).storageMode;
}

static MTLTextureDescriptor *
mtl_new_texture_descriptor(const struct kk_image_layout *layout)
{
   @autoreleasepool {
      MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
      descriptor.textureType = (MTLTextureType)layout->type;
      descriptor.pixelFormat = layout->format.mtl;
      descriptor.width = layout->width_px;
      descriptor.height = layout->height_px;
      descriptor.depth = layout->depth_px;
      descriptor.mipmapLevelCount = layout->levels;
      descriptor.sampleCount = layout->sample_count_sa;
      descriptor.arrayLength = layout->layers;
      descriptor.allowGPUOptimizedContents = layout->optimized_layout;
      descriptor.usage = (MTLTextureUsage)layout->usage;
      /* We don't set the swizzle because Metal complains when the usage has store or render target with swizzle... */
      
      return descriptor;
   }
}

mtl_texture *
mtl_new_memoryless_texture(mtl_device *device, const struct kk_image_layout *layout)
{
   @autoreleasepool {
      MTLTextureDescriptor *descriptor = [mtl_new_texture_descriptor(layout) autorelease];
      descriptor.storageMode = MTLStorageModeMemoryless;
      descriptor.usage = MTLTextureUsageRenderTarget;
      return [(id<MTLDevice>)device newTextureWithDescriptor:descriptor];
   }
}

/* A process-wide selector key also works when another instance of the driver
 * imports a retained heap after the original VkDeviceMemory has been freed.
 * The immutable bytes own no resource and introduce no allocation owner cycle. */
static const void *
mtl_heap_memory_region_key(void)
{
   return (const void *)sel_registerName("mesa.kosmickrisp.heap-memory-region.v1");
}

bool
mtl_heap_set_memory_region(mtl_heap *heap,
                            const struct mtl_heap_memory_region *region)
{
   @autoreleasepool {
      /* Only handle export publishes metadata. Concurrent exports publish once;
       * retained/imported aliases must never replace the original region. */
      @synchronized ((id)heap) {
         NSData *data = objc_getAssociatedObject((id)heap, mtl_heap_memory_region_key());
         if (data) {
            struct mtl_heap_memory_region existing;
            if (data.length != sizeof(existing))
               return false;
            [data getBytes:&existing length:sizeof(existing)];
            return existing.offset_B == region->offset_B &&
                   existing.size_B == region->size_B;
         }
         data = [[NSData alloc] initWithBytes:region length:sizeof(*region)];
         if (!data)
            return false;
         objc_setAssociatedObject((id)heap, mtl_heap_memory_region_key(), data,
                                   OBJC_ASSOCIATION_RETAIN_NONATOMIC);
         [data release];
         return true;
      }
   }
}

bool
mtl_heap_get_memory_region(mtl_heap *heap,
                            struct mtl_heap_memory_region *region)
{
   @autoreleasepool {
      id<MTLHeap> hp = (id<MTLHeap>)heap;
      @synchronized (hp) {
         NSData *data = objc_getAssociatedObject(hp, mtl_heap_memory_region_key());
         *region = (struct mtl_heap_memory_region){.size_B = hp.size};
         if (data) {
            if (data.length != sizeof(*region))
               return false;
            [data getBytes:region length:sizeof(*region)];
         }
         return region->offset_B <= hp.size &&
                region->size_B <= hp.size - region->offset_B;
      }
   }
}

/* Allocation from heap */
mtl_buffer *
mtl_new_buffer_with_length(mtl_heap *heap, uint64_t size_B, uint64_t offset_B)
{
   @autoreleasepool {
      id<MTLHeap> hp = (id<MTLHeap>)heap;
      return (mtl_buffer *)[hp newBufferWithLength:size_B options:hp.resourceOptions offset:offset_B];
   }
}

mtl_texture *
mtl_new_texture_with_descriptor(mtl_heap *heap,
                                const struct kk_image_layout *layout,
                                uint64_t offset)
{
   @autoreleasepool {
      id<MTLHeap> hp = (id<MTLHeap>)heap;
      MTLTextureDescriptor *descriptor = [mtl_new_texture_descriptor(layout) autorelease];
      descriptor.resourceOptions = hp.resourceOptions;
      return [hp newTextureWithDescriptor:descriptor offset:offset];
   }
}

void
mtl_heap_set_label(mtl_heap *heap, const char *label)
{
   @autoreleasepool {
      id<MTLHeap> h = (id<MTLHeap>)heap;
      h.label = @(label);
   }
}

void
mtl_resource_set_label(mtl_resource *resource, const char *label)
{
   @autoreleasepool {
      id<MTLResource> res = (id<MTLResource>)resource;
      res.label = @(label);
   }
}

#include "mtl_sparse_impl.h"
