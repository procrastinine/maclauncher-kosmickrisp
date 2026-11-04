/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_buffer.h"
#import <Foundation/Foundation.h>
#include <sys/mman.h>
#include <unistd.h>
#include "mtl_texture.h"

/* TODO_KOSMICKRISP Remove */
#include "kk_image_layout.h"

#include <Metal/Metal.h>
#include <Metal/MTLTexture.h>

/* A private allocation is not visible to any Vulkan queue until this
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
mtl_buffer_get_length(mtl_buffer *buffer)
{
   @autoreleasepool {
      id<MTLBuffer> buf = (id<MTLBuffer>)buffer;
      return buf.length;
   }
}

uint64_t
mtl_buffer_get_gpu_address(mtl_buffer *buffer)
{
   @autoreleasepool {
      id<MTLBuffer> buf = (id<MTLBuffer>)buffer;
      return [buf gpuAddress];
   }
}

void *
mtl_get_contents(mtl_buffer *buffer)
{
   @autoreleasepool {
      id<MTLBuffer> buf = (id<MTLBuffer>)buffer;
      return [buf contents];
   }
}

/* TODO_KOSMICKRISP This is a duplicate, but both should be removed once we move kk_image_layout to the bridge. */
static bool
mtl_buffer_needs_swizzle(const struct kk_image_layout *layout)
{
   return layout->type == MTL_TEXTURE_TYPE_TEXTURE_BUFFER &&
          (layout->swizzle.red != PIPE_SWIZZLE_X ||
           layout->swizzle.green != PIPE_SWIZZLE_Y ||
           layout->swizzle.blue != PIPE_SWIZZLE_Z ||
           layout->swizzle.alpha != PIPE_SWIZZLE_W);
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
      if (mtl_buffer_needs_swizzle(layout))
         descriptor.usage |= MTLTextureUsagePixelFormatView;
      /* We don't set the swizzle because Metal complains when the usage has store or render target with swizzle... */
      
      return descriptor;
   }
}

mtl_texture *
mtl_new_texture_with_descriptor_linear(mtl_buffer *buffer,
                                       const struct kk_image_layout *layout,
                                       uint64_t offset)
{
   @autoreleasepool {
      id<MTLBuffer> buf = (id<MTLBuffer>)buffer;
      MTLTextureDescriptor *descriptor = [mtl_new_texture_descriptor(layout) autorelease];
      descriptor.resourceOptions = buf.resourceOptions;
      id<MTLTexture> texture = [buf newTextureWithDescriptor:descriptor offset:offset bytesPerRow:layout->linear_stride_B];

      if (texture && mtl_buffer_needs_swizzle(layout)) {
         struct kk_view_layout view = {
            .sample_count_sa = 1,
            .format = {.pipe = layout->format.pipe, .mtl = layout->format.mtl},
            .num_levels = 1,
            .array_len = 1,
            .swizzle = {.red = layout->swizzle.red, .green = layout->swizzle.green,
                        .blue = layout->swizzle.blue, .alpha = layout->swizzle.alpha},
         };
         mtl_texture *swizzled = mtl_new_texture_view_with(texture, &view);
         [texture release];
         return swizzled;
      }
      return texture;
   }
}


mtl_buffer *
mtl_new_buffer_with_memory_fd(mtl_device *device, int fd, void *mapping,
                               uint64_t mapping_size, uint64_t payload_offset,
                               uint64_t payload_size)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;
      id<MTLBuffer> buffer = [dev newBufferWithBytesNoCopy:(char *)mapping + payload_offset
         length:payload_size options:MTLResourceStorageModeShared |
                                       MTLResourceHazardTrackingModeUntracked
         deallocator:^(void *pointer, NSUInteger length) {
            munmap(mapping, mapping_size);
            close(fd);
         }];
      return (mtl_buffer *)buffer;
   }
}
