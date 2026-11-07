/* SPDX-License-Identifier: MIT */
#import <Metal/Metal.h>
#include "kosmickrisp-rt-metal.h"
#include <stdio.h>
#include <string.h>

bool
kk_rt_link_functions(mtl_compiler *compiler,
                      mtl_function_descriptor *dispatcher,
                      uint32_t function_count,
                      mtl_function_descriptor *const *functions,
                      uint32_t max_threads,
                      struct kk_rt_native_pipeline *out)
{
   memset(out, 0, sizeof(*out));
   @autoreleasepool {
      NSMutableArray<MTL4FunctionDescriptor *> *linked =
         [NSMutableArray arrayWithCapacity:function_count];
      NSMutableSet<NSString *> *names = [NSMutableSet set];
      for (uint32_t i = 0; i < function_count; ++i) {
         if (!functions[i])
            return false;
         MTL4LibraryFunctionDescriptor *function = (MTL4LibraryFunctionDescriptor *)functions[i];
         if (![names containsObject:function.name]) {
            [names addObject:function.name];
            [linked addObject:function];
         }
      }
      MTL4StaticLinkingDescriptor *link =
         [[MTL4StaticLinkingDescriptor new] autorelease];
      link.functionDescriptors = linked;
      MTL4ComputePipelineDescriptor *descriptor =
         [[MTL4ComputePipelineDescriptor new] autorelease];
      descriptor.computeFunctionDescriptor = (MTL4FunctionDescriptor *)dispatcher;
      descriptor.maxTotalThreadsPerThreadgroup = max_threads;
      descriptor.staticLinkingDescriptor = link;
      /* Vulkan recursion suspends to the dispatcher; visible functions never
       * recursively call each other. This is the shallow native call depth. */
      MTL4PipelineStageDynamicLinkingDescriptor *dynamic =
         [[MTL4PipelineStageDynamicLinkingDescriptor new] autorelease];
      dynamic.maxCallStackDepth = 2;
      NSError *error = nil;
      id<MTLComputePipelineState> pipeline = [(id<MTL4Compiler>)compiler
         newComputePipelineStateWithDescriptor:descriptor
         dynamicLinkingDescriptor:dynamic compilerTaskOptions:nil error:&error];
      if (!pipeline) {
         fprintf(stderr, "RT pipeline link failed: %s\n", error.localizedDescription.UTF8String);
         return false;
      }
      MTLVisibleFunctionTableDescriptor *table_descriptor =
         [[MTLVisibleFunctionTableDescriptor new] autorelease];
      table_descriptor.functionCount = function_count;
      id<MTLVisibleFunctionTable> table =
         [pipeline newVisibleFunctionTableWithDescriptor:table_descriptor];
      if (!table) {
         [pipeline release];
         return false;
      }
      for (uint32_t i = 0; i < function_count; ++i) {
         MTL4LibraryFunctionDescriptor *function = (MTL4LibraryFunctionDescriptor *)functions[i];
         id<MTLFunctionHandle> handle = [pipeline functionHandleWithName:function.name];
         if (!handle) {
            [table release];
            [pipeline release];
            return false;
         }
         [table setFunction:handle atIndex:i];
      }
      out->pipeline = (mtl_compute_pipeline_state *)pipeline;
      out->function_table = (mtl_allocation *)table;
      out->function_table_id = table.gpuResourceID._impl;
      return true;
   }
}

void
kk_rt_native_pipeline_finish(struct kk_rt_native_pipeline *pipeline)
{
   [(id<MTLVisibleFunctionTable>)pipeline->function_table release];
   [(id<MTLComputePipelineState>)pipeline->pipeline release];
   memset(pipeline, 0, sizeof(*pipeline));
}
