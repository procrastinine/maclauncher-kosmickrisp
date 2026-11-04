/* SPDX-License-Identifier: MIT AND Apache-2.0
 * Copyright (c) 2015-2026 The Brenwill Workshop Ltd.
 * Placement sparse resource and mip-tail technique follows MetalSharp's
 * MoltenVK adaptation; source and notices are recorded in sparse-origins.json.
 * Included in mtl_heap.m to share its Metal texture descriptor construction.
 */
#include "mtl_sparse.h"
#include "kk_format.h"
#include <string.h>
#import <Metal/Metal.h>

bool
mtl_sparse_commit_mapping_barrier(mtl_device *device, mtl_command_queue *queue,
   mtl_command_buffer **command, mtl_command_allocator **allocator,
   const struct mtl_sparse_residency_copy *copies, uint32_t copy_count,
   mtl_feedback_handler_callback callback, void *data)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;
      id<MTL4CommandAllocator> alloc = [dev newCommandAllocator];
      id<MTL4CommandBuffer> cmd = [dev newCommandBuffer];
      if (!alloc || !cmd) { [alloc release]; [cmd release]; return false; }
      [cmd beginCommandBufferWithAllocator:alloc];
      id<MTL4ComputeCommandEncoder> enc = [cmd computeCommandEncoder];
      if (!enc) {
         [cmd endCommandBuffer]; [cmd release]; [alloc release]; return false;
      }
      /* Publish page-table and alias-cache changes before later queue work.
       * Buffer residency query bytes follow the mapping on the GPU. */
      [enc barrierAfterQueueStages:MTLStageResourceState beforeStages:MTLStageAll
         visibilityOptions:MTL4VisibilityOptionResourceAlias | MTL4VisibilityOptionDevice];
      for (unsigned i = 0; i < copy_count; ++i)
         [enc copyFromBuffer:(id<MTLBuffer>)copies[i].source sourceOffset:0
            toBuffer:(id<MTLBuffer>)copies[i].destination
            destinationOffset:copies[i].destination_offset size:copies[i].size];
      [enc barrierAfterStages:MTLStageResourceState | MTLStageBlit beforeQueueStages:MTLStageAll
         visibilityOptions:MTL4VisibilityOptionResourceAlias | MTL4VisibilityOptionDevice];
      [enc endEncoding];
      [cmd endCommandBuffer];
      mtl_commit_options *options = mtl_new_commit_options();
      if (!options) { [cmd release]; [alloc release]; return false; }
      mtl_commit_options_add_feedback_handler(options, callback, data);
      *command = (mtl_command_buffer *)cmd;
      *allocator = (mtl_command_allocator *)alloc;
      [(id<MTL4CommandQueue>)queue commit:&cmd count:1 options:(MTL4CommitOptions *)options];
      [(id)options release];
      return true;
   }
}

mtl_buffer *
mtl_sparse_new_residency_buffer(mtl_device *device, uint64_t size, bool resident)
{
   id<MTLBuffer> buffer = [(id<MTLDevice>)device newBufferWithLength:size
      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
   if (buffer) memset(buffer.contents, resident ? 1 : 0, size);
   return (mtl_buffer *)buffer;
}

bool
mtl_device_supports_placement_sparse(mtl_device *device)
{
   id<MTLDevice> dev = (id<MTLDevice>)device;
   if (@available(macOS 26.4, *))
      return dev.supportsPlacementSparse &&
         [dev sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64] == 65536;
   /* The capability query was added after placement sparse resources.
    * Apple's Metal Feature Set Tables list placement sparse buffers and
    * textures for every Apple7 macOS device and for all Apple8 and later
    * GPUs. */
   if (@available(macOS 26.0, *))
      return [dev supportsFamily:MTLGPUFamilyApple7] &&
         [dev sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64] == 65536;
   return false;
}

struct mtl_size
mtl_sparse_granularity(mtl_device *device, uint32_t format, enum mtl_texture_type type)
{
   MTLSize tile = [(id<MTLDevice>)device sparseTileSizeWithTextureType:(MTLTextureType)type
      pixelFormat:(MTLPixelFormat)format sampleCount:1 sparsePageSize:MTLSparsePageSize64];
   return (struct mtl_size){tile.width, tile.height, tile.depth};
}

mtl_buffer *
mtl_new_sparse_buffer(mtl_device *device, uint64_t size)
{
   return [(id<MTLDevice>)device newBufferWithLength:size
      options:MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked
      placementSparsePageSize:MTLSparsePageSize64];
}

mtl_texture *
mtl_new_sparse_texture(mtl_device *device, const struct kk_image_layout *layout)
{
   @autoreleasepool {
      MTLTextureDescriptor *desc = [mtl_new_texture_descriptor(layout) autorelease];
      desc.resourceOptions = MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
      desc.placementSparsePageSize = MTLSparsePageSize64;
      /* Shader writes select sparse tier 1. Tier 1 residency feedback is
       * wrong for 2D tiles that are not square (2- and 8-byte texels), so
       * 2D textures add shader writes only when Vulkan usage needs them and
       * otherwise remain tier 2. 3D textures keep tier 1. */
      const struct kk_va_format *format = kk_get_va_format(layout->format.pipe);
      if (format->write && layout->type == MTL_TEXTURE_TYPE_3D)
         desc.usage |= MTLTextureUsageShaderWrite;
      return [(id<MTLDevice>)device newTextureWithDescriptor:desc];
   }
}

uint64_t
mtl_sparse_tail_size(mtl_texture *texture)
{
   return [(id<MTLTexture>)texture tailSizeInBytes];
}

/* The mip tail of the same texture without shader writes (sparse tier 2), as
 * a sampled-only sparse image of this shape has it. Placement sparse
 * textures have no backing, so the probe texture costs no memory. */
void
mtl_sparse_shader_read_tail(mtl_device *device, const struct kk_image_layout *layout,
                            uint32_t *first_tail, uint64_t *tail_size)
{
   @autoreleasepool {
      MTLTextureDescriptor *desc = [mtl_new_texture_descriptor(layout) autorelease];
      desc.resourceOptions = MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeUntracked;
      desc.placementSparsePageSize = MTLSparsePageSize64;
      desc.usage &= ~(MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic);
      id<MTLTexture> texture = [(id<MTLDevice>)device newTextureWithDescriptor:desc];
      *first_tail = texture ? (uint32_t)[texture firstMipmapInTail] : UINT32_MAX;
      *tail_size = texture ? [texture tailSizeInBytes] : 0;
      [texture release];
   }
}

void *
mtl_new_sparse_buffer_view(mtl_buffer *buffer, const struct kk_image_layout *layout,
                           uint64_t offset, uint64_t *resource_id)
{
   @autoreleasepool {
      id<MTLBuffer> buf = (id<MTLBuffer>)buffer;
      MTLResourceViewPoolDescriptor *pool_desc = [MTLResourceViewPoolDescriptor new];
      pool_desc.resourceViewCount = 1;
      id<MTLTextureViewPool> pool = [buf.device newTextureViewPoolWithDescriptor:pool_desc error:NULL];
      [pool_desc release];
      if (!pool) return NULL;
      MTLTextureDescriptor *desc = [mtl_new_texture_descriptor(layout) autorelease];
      desc.resourceOptions = buf.resourceOptions;
      desc.placementSparsePageSize = MTLSparsePageSize64;
      MTLResourceID view = [pool setTextureViewFromBuffer:buf descriptor:desc
         offset:offset bytesPerRow:layout->linear_stride_B atIndex:0];
      *resource_id = view._impl;
      if (!view._impl) { [pool release]; return NULL; }
      return pool;
   }
}

/* The C bridge structures mirror the SDK mapping ABI on both macOS CPUs. */
_Static_assert(sizeof(struct mtl_sparse_buffer_mapping) == sizeof(MTL4UpdateSparseBufferMappingOperation), "buffer mapping ABI");
_Static_assert(offsetof(struct mtl_sparse_buffer_mapping, first) == offsetof(MTL4UpdateSparseBufferMappingOperation, bufferRange), "buffer range ABI");
_Static_assert(offsetof(struct mtl_sparse_buffer_mapping, offset) == offsetof(MTL4UpdateSparseBufferMappingOperation, heapOffset), "buffer offset ABI");
_Static_assert(sizeof(struct mtl_sparse_texture_mapping) == sizeof(MTL4UpdateSparseTextureMappingOperation), "texture mapping ABI");
_Static_assert(offsetof(struct mtl_sparse_texture_mapping, origin) == offsetof(MTL4UpdateSparseTextureMappingOperation, textureRegion), "texture region ABI");
_Static_assert(offsetof(struct mtl_sparse_texture_mapping, level) == offsetof(MTL4UpdateSparseTextureMappingOperation, textureLevel), "texture level ABI");
_Static_assert(offsetof(struct mtl_sparse_texture_mapping, offset) == offsetof(MTL4UpdateSparseTextureMappingOperation, heapOffset), "texture offset ABI");

void
mtl_sparse_map_buffer_batch(mtl_command_queue *queue, mtl_buffer *buffer, mtl_heap *heap,
   const struct mtl_sparse_buffer_mapping *ops, uint32_t count)
{
   [(id<MTL4CommandQueue>)queue updateBufferMappings:(id<MTLBuffer>)buffer
      heap:(id<MTLHeap>)heap operations:(const MTL4UpdateSparseBufferMappingOperation *)ops count:count];
}

void
mtl_sparse_map_texture_batch(mtl_command_queue *queue, mtl_texture *texture, mtl_heap *heap,
   const struct mtl_sparse_texture_mapping *ops, uint32_t count)
{
   [(id<MTL4CommandQueue>)queue updateTextureMappings:(id<MTLTexture>)texture
      heap:(id<MTLHeap>)heap operations:(const MTL4UpdateSparseTextureMappingOperation *)ops count:count];
}

