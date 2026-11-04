/* SPDX-License-Identifier: MIT */
#include "kk_dgc.h"
#include "kk_bo.h"
#include "kk_device.h"
#include "kk_cmd_buffer.h"
#include "kk_shader.h"
#include "kosmickrisp/bridge/mtl_compiler.h"
#include "util/simple_mtx.h"
#include "kk_dgc_shader.h"
#import <Metal/Metal.h>
#include <stdio.h>

struct kk_dgc_metal {
   id<MTLLibrary> library;
   mtl_compute_pipeline_state *pipeline[4];
   simple_mtx_t pipeline_mutex;
};

static bool kk_dgc_create(struct kk_device *dev)
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
      dgc->library = lib;
      simple_mtx_init(&dgc->pipeline_mutex, mtx_plain);
      p_atomic_set(&dev->dgc, dgc);
      return true;
   }
}
static void kk_dgc_init_once(const void *data)
{
   kk_dgc_create((struct kk_device *)data);
}

bool kk_dgc_init(struct kk_device *dev)
{
   /* No library or pipeline compilation until a generated operation needs it.
    * The atomic read also acquires publication after the once fast path. */
   util_call_once_data(&dev->dgc_once, kk_dgc_init_once, dev);
   return p_atomic_read(&dev->dgc) != NULL;
}

void kk_dgc_finish(struct kk_device *dev)
{
   if (!dev->dgc)
      return;
   for (unsigned i = 0; i < 4; ++i)
      [(id<MTLComputePipelineState>)dev->dgc->pipeline[i] release];
   [dev->dgc->library release];
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
static id<MTLComputePipelineState>
kk_dgc_phase_pipeline(struct kk_device *dev, unsigned phase)
{
   struct kk_dgc_metal *dgc = dev->dgc;
   id<MTLComputePipelineState> pipeline = (id<MTLComputePipelineState>)p_atomic_read(&dgc->pipeline[phase]);
   if (pipeline) return pipeline;
   simple_mtx_lock(&dgc->pipeline_mutex);
   pipeline = (id<MTLComputePipelineState>)dgc->pipeline[phase];
   if (!pipeline) {
      static NSString *const names[] = {
         @"kk_dgc_preprocess", @"kk_dgc_expand", @"kk_dgc_commands",
         @"kk_dgc_offset_count"
      };
      MTL4LibraryFunctionDescriptor *fn = [MTL4LibraryFunctionDescriptor new];
      fn.library = dgc->library;
      fn.name = names[phase];
      MTL4ComputePipelineDescriptor *pd = [MTL4ComputePipelineDescriptor new];
      pd.computeFunctionDescriptor = fn;
      NSError *error = nil;
      pipeline = [(id<MTL4Compiler>)dev->mtl_compiler_handle
         newComputePipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&error];
      [pd release];
      [fn release];
      if (pipeline)
         p_atomic_set(&dgc->pipeline[phase], (mtl_compute_pipeline_state *)pipeline);
      else
         fprintf(stderr, "DGC pipeline: %s\n", error.description.UTF8String);
   }
   simple_mtx_unlock(&dgc->pipeline_mutex);
   return pipeline;
}

bool kk_dgc_encode(struct kk_cmd_buffer *cmd, mtl_compute_encoder *encoder,
                   uint64_t parameters, uint32_t count,
                   uint32_t phase)
{
   @autoreleasepool {
      id<MTL4ComputeCommandEncoder> enc =
          (id<MTL4ComputeCommandEncoder>)encoder;
      struct kk_device *dev = kk_cmd_buffer_device(cmd);
      id<MTLComputePipelineState> pipeline = kk_dgc_phase_pipeline(dev, phase);
      if (!pipeline) {
         vk_command_buffer_set_error(&cmd->vk, VK_ERROR_INVALID_SHADER_NV);
         return false;
      }
      /* Dispatch snapshots the table's bindings, as for ordinary libkk
       * dispatches. Reuse it and restore the two overwritten entries below. */
      id<MTL4ArgumentTable> table = (id<MTL4ArgumentTable>)cmd->argument_table;
      [table setAddress:parameters atIndex:KK_ARGUMENT_ROOT];
      if (phase == 2)
         [table setAddress:parameters + sizeof(struct kk_dgc_params) atIndex:KK_ARGUMENT_SAMPLERS];
      [enc setArgumentTable:table];
      [enc setComputePipelineState:pipeline];
      [enc dispatchThreads:MTLSizeMake(count, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
      [enc barrierAfterEncoderStages:MTLStageDispatch
                 beforeEncoderStages:MTLStageDispatch
                   visibilityOptions:MTL4VisibilityOptionResourceAlias];
      [table setAddress:cmd->state.root_addr atIndex:KK_ARGUMENT_ROOT];
      if (phase == 2)
         [table setAddress:dev->samplers.table.bo->gpu atIndex:KK_ARGUMENT_SAMPLERS];
      return true;
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
   mtl_compute_pipeline_state *cached = p_atomic_read(&shader->dgc_compute);
   if (cached) return cached;
   if (!kk_dgc_init(dev)) return NULL;
   simple_mtx_lock(&dev->dgc->pipeline_mutex);
   if (!shader->dgc_compute) {
      uint32_t threads = shader->info.cs.local_size.x * shader->info.cs.local_size.y * shader->info.cs.local_size.z;
      mtl_compute_pipeline_state *pipeline = NULL;
      kk_compile_indirect_compute_pipeline(dev, &shader->msl_data[MESA_SHADER_COMPUTE], threads, &pipeline);
      p_atomic_set(&shader->dgc_compute, pipeline);
   }
   mtl_compute_pipeline_state *pipeline = shader->dgc_compute;
   simple_mtx_unlock(&dev->dgc->pipeline_mutex);
   return pipeline;
}
