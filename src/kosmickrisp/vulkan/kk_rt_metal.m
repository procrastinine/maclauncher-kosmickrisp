/* SPDX-License-Identifier: MIT */
#import <Metal/Metal.h>
#include "kosmickrisp-rt-metal.h"
#include <stdio.h>
#include <string.h>

/* Sampler emulation and 64-bit atomic locks specialize function constants
 * over a library function. The linked function keeps the library function's
 * name unless the specialization names it. */
static NSString *
kk_rt_function_name(MTL4FunctionDescriptor *function)
{
   while ([function isKindOfClass:[MTL4SpecializedFunctionDescriptor class]]) {
      MTL4SpecializedFunctionDescriptor *specialized = (id)function;
      if (specialized.specializedName.length)
         return specialized.specializedName;
      function = specialized.functionDescriptor;
   }
   if (![function isKindOfClass:[MTL4LibraryFunctionDescriptor class]])
      return nil;
   return ((MTL4LibraryFunctionDescriptor *)function).name;
}

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
         MTL4FunctionDescriptor *function = (MTL4FunctionDescriptor *)functions[i];
         NSString *name = kk_rt_function_name(function);
         if (!name.length)
            return false;
         if (![names containsObject:name]) {
            [names addObject:name];
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
         id<MTLFunctionHandle> handle = [pipeline functionHandleWithName:
            kk_rt_function_name((MTL4FunctionDescriptor *)functions[i])];
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
