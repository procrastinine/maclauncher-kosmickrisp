/* SPDX-License-Identifier: MIT */
#include "kk_dgc.h"
#include "kk_device.h"
#include "kk_shader.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "util/simple_mtx.h"
#include "kk_dgc_shader.h"
#import <Metal/Metal.h>
#include <stdio.h>

struct kk_dgc_metal {
   id<MTLComputePipelineState> pipeline[4];
   simple_mtx_t pipeline_mutex;
};

bool kk_dgc_init(struct kk_device *dev)
{
   @autoreleasepool {
      struct kk_dgc_metal *dgc = calloc(1, sizeof(*dgc));
      if (!dgc)
         return false;
      id<MTL4Compiler> compiler = (id<MTL4Compiler>)dev->mtl_compiler_handle;
      MTL4LibraryDescriptor *ld = [MTL4LibraryDescriptor new];
      ld.source = [NSString stringWithFormat:@"%s%s",
         KK_ARGUMENT_TABLE_MSL_PROLOG, kk_dgc_shader];
      MTLCompileOptions *options = [MTLCompileOptions new];
      options.languageVersion = MTLLanguageVersion3_1;
      ld.options = options;
      [options release];
      NSError *error = nil;
      id<MTLLibrary> lib = [compiler newLibraryWithDescriptor:ld error:&error];
      [ld release];
      if (!lib) {
         fprintf(stderr, "DGC library: %s\n", error.description.UTF8String);
         free(dgc);
         return false;
      }
      NSArray<NSString *> *names = @[
         @"kk_dgc_preprocess", @"kk_dgc_expand", @"kk_dgc_commands",
         @"kk_dgc_offset_count"
      ];
      for (unsigned i = 0; i < 4; ++i) {
         MTL4LibraryFunctionDescriptor *fn =
             [MTL4LibraryFunctionDescriptor new];
         fn.library = lib;
         fn.name = names[i];
         MTL4ComputePipelineDescriptor *pd =
             [MTL4ComputePipelineDescriptor new];
         pd.computeFunctionDescriptor = fn;
         id<MTLComputePipelineState> pipeline =
             [compiler newComputePipelineStateWithDescriptor:pd
                                         compilerTaskOptions:nil
                                                       error:&error];
         [fn release];
         [pd release];
         dgc->pipeline[i] = pipeline;
         if (!pipeline) {
            fprintf(stderr, "DGC pipeline: %s\n", error.description.UTF8String);
            for (unsigned j = 0; j < 4; ++j)
               [dgc->pipeline[j] release];
            [lib release];
            free(dgc);
            return false;
         }
      }
      [lib release];
      simple_mtx_init(&dgc->pipeline_mutex, mtx_plain);
      dev->dgc = dgc;
      return true;
   }
}
void kk_dgc_finish(struct kk_device *dev)
{
   if (!dev->dgc)
      return;
   for (unsigned i = 0; i < 4; ++i)
      [dev->dgc->pipeline[i] release];
   simple_mtx_destroy(&dev->dgc->pipeline_mutex);
   free(dev->dgc);
   dev->dgc = NULL;
}
void *kk_dgc_new_icb(struct kk_device *dev, uint32_t count, bool compute)
{
   @autoreleasepool {
      MTLIndirectCommandBufferDescriptor *desc =
          [MTLIndirectCommandBufferDescriptor new];
      desc.commandTypes = compute ? MTLIndirectCommandTypeConcurrentDispatch
                                  : (MTLIndirectCommandTypeDraw |
                                     MTLIndirectCommandTypeDrawIndexed);
      desc.inheritPipelineState = YES;
      desc.inheritBuffers = NO;
      const unsigned buffer_count = kk_argument_table_buffer_count(
         (struct kk_argument_table_usage){
            .linked_graphics = !compute,
            .poly_layer_replay = !compute,
            .interlock_guard = !compute && dev->interlock_guard != NULL,
         });
      desc.maxVertexBufferBindCount = buffer_count;
      desc.maxFragmentBufferBindCount = buffer_count;
      desc.maxKernelBufferBindCount = buffer_count;
      id<MTLIndirectCommandBuffer> icb = [(id<MTLDevice>)dev->mtl_handle
          newIndirectCommandBufferWithDescriptor:desc
                                 maxCommandCount:count
                                         options:MTLResourceStorageModePrivate];
      [desc release];
      return icb;
   }
}
uint64_t kk_dgc_icb_id(void *icb)
{
   return ((id<MTLIndirectCommandBuffer>)icb).gpuResourceID._impl;
}
void kk_dgc_encode(struct kk_device *dev, mtl_compute_encoder *encoder,
                   uint64_t parameters, void *icb, uint32_t count,
                   uint32_t phase)
{
   @autoreleasepool {
      id<MTL4ComputeCommandEncoder> enc =
          (id<MTL4ComputeCommandEncoder>)encoder;
      MTL4ArgumentTableDescriptor *td = [MTL4ArgumentTableDescriptor new];
      td.maxBufferBindCount = kk_argument_table_buffer_count(
         (struct kk_argument_table_usage){0});
      id<MTL4ArgumentTable> table =
          [(id<MTLDevice>)dev->mtl_handle newArgumentTableWithDescriptor:td
                                                                   error:nil];
      [td release];
      [table setAddress:parameters atIndex:KK_ARGUMENT_ROOT];
      [table setAddress:parameters + sizeof(struct kk_dgc_params) atIndex:KK_ARGUMENT_SAMPLERS];
      [enc setArgumentTable:table];
      [enc setComputePipelineState:dev->dgc->pipeline[phase]];
      [enc dispatchThreads:MTLSizeMake(count, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
      [enc barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageDispatch
                   visibilityOptions:MTL4VisibilityOptionResourceAlias];
      [table release];
   }
}
void kk_dgc_execute_compute(mtl_compute_encoder *encoder, void *icb,
                            uint64_t range,
                            mtl_compute_pipeline_state *pipeline)
{
   id<MTL4ComputeCommandEncoder> enc = (id<MTL4ComputeCommandEncoder>)encoder;
   [enc setComputePipelineState:(id<MTLComputePipelineState>)pipeline];
   [enc executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)icb
                 indirectBuffer:range];
}
void kk_dgc_execute_graphics(mtl_render_encoder *encoder, void *icb,
                             uint64_t range)
{
   [(id<MTL4RenderCommandEncoder>)encoder
       executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)icb
                indirectBuffer:range];
}

mtl_render_pipeline_descriptor *kk_dgc_render_descriptor(mtl_render_pipeline_descriptor *source)
{
   MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)source copy];
   if (!copy) return NULL;
   mtl_render_pipeline_descriptor_copy_linking((void *)copy, source);
   copy.supportIndirectCommandBuffers = MTL4IndirectCommandBufferSupportStateEnabled;
   return (void *)copy;
}
mtl_compute_pipeline_state *kk_dgc_new_compute_pipeline_state(mtl_compiler *compiler, mtl_function_descriptor *function, uint64_t threads)
{
   @autoreleasepool {
      MTL4ComputePipelineDescriptor *desc = [MTL4ComputePipelineDescriptor new];
      desc.computeFunctionDescriptor = (MTL4FunctionDescriptor *)function;
      desc.maxTotalThreadsPerThreadgroup = threads;
      desc.supportIndirectCommandBuffers = MTL4IndirectCommandBufferSupportStateEnabled;
      NSError *error = nil;
      id<MTLComputePipelineState> pipeline = [(id<MTL4Compiler>)compiler newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error];
      [desc release];
      if (!pipeline) fprintf(stderr,"DGC compute pipeline: %s\n",error.description.UTF8String);
      return (void *)pipeline;
   }
}
mtl_compute_pipeline_state *kk_dgc_get_compute_pipeline(struct kk_device *dev, struct kk_shader *shader)
{
   if (!dev->dgc) return NULL;
   simple_mtx_lock(&dev->dgc->pipeline_mutex);
   if (!shader->dgc_compute) {
      uint32_t threads = shader->info.cs.local_size.x * shader->info.cs.local_size.y * shader->info.cs.local_size.z;
      kk_compile_indirect_compute_pipeline(dev, &shader->msl_data[MESA_SHADER_COMPUTE], threads, &shader->dgc_compute);
   }
   mtl_compute_pipeline_state *pipeline = shader->dgc_compute;
   simple_mtx_unlock(&dev->dgc->pipeline_mutex);
   return pipeline;
}
