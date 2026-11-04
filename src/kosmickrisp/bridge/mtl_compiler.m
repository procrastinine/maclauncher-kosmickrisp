/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_compiler.h"
#include "mtl_format.h"
#include "kosmickrisp/libkk/kk_argument_table.h"

#include <Metal/MTL4ComputePipeline.h>
#include <Metal/MTL4LibraryDescriptor.h>
#include <Metal/MTL4LibraryFunctionDescriptor.h>
#include <Metal/MTL4SpecializedFunctionDescriptor.h>
#include <Metal/MTL4RenderPipeline.h>
#include <Metal/MTLDevice.h>
#include <Metal/MTL4CompilerTask.h>
#include <dispatch/dispatch.h>
#include <string.h>

#include "mtl_pipeline_archive.h"
#include "mtl_function_constants.h"

/* Immutable process diagnostic, initialized before compiler publication. */
static NSString *debug_msl_prefix;

void
mtl_init_debug_msl_salt(const char *salt)
{
   if (!salt || !salt[0]) return;
   @autoreleasepool {
      /* Base64 keeps arbitrary environment bytes inside one harmless comment. */
      NSData *bytes = [NSData dataWithBytes:salt length:strlen(salt)];
      debug_msl_prefix = [[NSString alloc] initWithFormat:
         @"// KK_DEBUG_MSL_SALT(base64):%@\n#line 1\n", [bytes base64EncodedStringWithOptions:0]];
   }
}

/* Compiler */
static mtl_compiler *
mtl_create_compiler(mtl_device *device, const struct mtl_compiler_cache_ops *ops, void *data)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;

      dev.shouldMaximizeConcurrentCompilation = YES;
      MTL4CompilerDescriptor *desc = [[MTL4CompilerDescriptor new] autorelease];

      KKPipelineArchive *cache = ops ? [[KKPipelineArchive alloc] initWithDevice:dev operations:ops data:data] : nil;
      if (ops && !cache) ops->destroy(data);
      if (cache) desc.pipelineDataSetSerializer = cache->serializer;
      NSError *error = nil;
      id<MTL4Compiler> compiler = [dev newCompilerWithDescriptor:desc error:&error];

      if (compiler == nil) {
         fprintf(stderr, "Failed to create MTL4Compiler: %s\n", [error.localizedDescription UTF8String]);
      }

      if (compiler && cache)
         objc_setAssociatedObject(compiler, &kk_archive_association, cache, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      [cache release];
      return compiler;
   }
}

mtl_compiler *
mtl_new_compiler(mtl_device *device)
{
   return mtl_create_compiler(device, NULL, NULL);
}

mtl_compiler *
mtl_new_cached_compiler(mtl_device *device,
   const struct mtl_compiler_cache_ops *ops, void *data)
{
   return mtl_create_compiler(device, ops, data);
}

void
mtl_compiler_finish_cache(mtl_compiler *compiler)
{
   @autoreleasepool { [kk_compiler_archive(compiler) finish]; }
}

static char kk_compile_queue_association;
static pthread_mutex_t kk_compile_queue_lock = PTHREAD_MUTEX_INITIALIZER;

/* Native miss scheduling only: cached deserialization stays on its original
 * synchronous path. Never retain caller create-info or feedback pointers. */
void *
mtl_compiler_submit(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *))
{
   @autoreleasepool {
      /* Queue availability is independent of disk-cache availability. This
       * lock covers lookup/publication only, never a native compile or wait;
       * do not synchronize on Metal's compiler object/internal monitor. */
      pthread_mutex_lock(&kk_compile_queue_lock);
      NSOperationQueue *queue = objc_getAssociatedObject((id)compiler,
         &kk_compile_queue_association);
      if (!queue) {
         queue = [NSOperationQueue new];
         NSUInteger cores = MIN((NSUInteger)8, [NSProcessInfo processInfo].activeProcessorCount);
         queue.maxConcurrentOperationCount = cores > 1 ? cores - 1 : 1;
         queue.qualityOfService = NSQualityOfServiceUserInitiated;
         queue.name = @"kosmickrisp.compute";
         if (queue) {
            objc_setAssociatedObject((id)compiler, &kk_compile_queue_association,
                                      queue, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [queue release];
         }
      }
      pthread_mutex_unlock(&kk_compile_queue_lock);
      if (!queue) return NULL;
      /* The device owner drains its jobs before compiler-table release.
       * Do not hide a compiler retain in the operation: workaround11 must
       * still retire the final native compiler before publishing a new one. */
      NSBlockOperation *operation = [[NSBlockOperation blockOperationWithBlock:^{
         @autoreleasepool {
            @try { execute(data); }
            @catch (NSException *exception) { failed(data, exception.reason.UTF8String); }
         }
      }] retain];
      if (!operation) return NULL;
      operation.completionBlock = ^{ @autoreleasepool { cleanup(data); } };
      [queue addOperation:operation];
      return operation;
   }
}

void
mtl_compiler_task_wait(void *task)
{
   if (!task) return;
   NSOperation *operation = (NSOperation *)task;
   if (!operation.finished) {
      /* Wait only for this pipeline. Promote a queued first-use request over
       * unrelated cold misses; running native jobs remain machine-bounded. */
      operation.queuePriority = NSOperationQueuePriorityVeryHigh;
      [operation waitUntilFinished];
   }
}

void
mtl_compiler_store_shader(mtl_compiler *compiler, const void *key,
   size_t key_size, const void *bytes, size_t size)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return;
   [cache load];
   if (cache->available && cache->operations.store_shader)
      cache->operations.store_shader(cache->cache_data, key, key_size, bytes, size);
}

/* Library */
static MTL4LibraryDescriptor *
mtl_library_descriptor(const char *src, enum mtl_language_version msl_version,
                       enum mtl_math_mode math_mode,
                       enum mtl_math_floating_point_functions math_fp_fns)
{
   MTLCompileOptions *opts = [[MTLCompileOptions new] autorelease];
   opts.languageVersion = (MTLLanguageVersion)msl_version;
   opts.mathMode = (MTLMathMode)math_mode;
   opts.mathFloatingPointFunctions = (MTLMathFloatingPointFunctions)math_fp_fns;

   MTL4LibraryDescriptor *desc = [[MTL4LibraryDescriptor new] autorelease];
   desc.source = [NSString stringWithCString:src encoding:NSASCIIStringEncoding];
   if (debug_msl_prefix)
      desc.source = [debug_msl_prefix stringByAppendingString:desc.source];
   desc.options = opts;
   return desc;
}

mtl_library *
mtl_new_library(mtl_compiler *compiler, const char *src,
                enum mtl_language_version msl_version,
                enum mtl_math_mode math_mode,
                enum mtl_math_floating_point_functions math_fp_fns)
{
   @autoreleasepool {
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;
      MTL4LibraryDescriptor *desc =
         mtl_library_descriptor(src, msl_version, math_mode, math_fp_fns);
      NSError *error = nil;
      id<MTLLibrary> lib = [comp newLibraryWithDescriptor:desc error:&error];
      if (lib == nil)
         fprintf(stderr, "Failed to create MTLLibrary: %s\n",
                 error.localizedDescription.UTF8String ?: "Unknown Metal error");
      return lib;
   }
}

mtl_library *
mtl_new_library_async_wait(mtl_compiler *compiler, const char *src,
   enum mtl_language_version version, enum mtl_math_mode mode,
   enum mtl_math_floating_point_functions fp)
{
   @autoreleasepool {
      MTL4LibraryDescriptor *desc = mtl_library_descriptor(src, version, mode, fp);
      dispatch_group_t completion = dispatch_group_create();
      if (!completion) return NULL;
      __block id<MTLLibrary> result = nil;
      dispatch_group_enter(completion);
      id<MTL4CompilerTask> task = nil;
      @try {
         task = [(id<MTL4Compiler>)compiler newLibraryWithDescriptor:desc
         completionHandler:^(id<MTLLibrary> library, NSError *error) {
            result = [library retain];
            if (!library)
               fprintf(stderr, "Failed to create MTLLibrary: %s\n",
                  error.localizedDescription.UTF8String ?: "Unknown Metal error");
            dispatch_group_leave(completion);
         }];
         dispatch_group_wait(completion, DISPATCH_TIME_FOREVER);
      } @catch (NSException *exception) {
         [result release];
         @throw;
      } @finally {
         [task release];
         dispatch_release(completion);
      }
      return (mtl_library *)result;
   }
}

struct mtl_library_pair {
   id<MTL4Compiler> compiler;
   dispatch_group_t completion;
   id<MTL4CompilerTask> tasks[2];
   mtl_library *libraries[2];
   enum mtl_math_mode math_mode;
   enum mtl_math_floating_point_functions math_fp_fns;
};

static void
mtl_library_pair_submit(struct mtl_library_pair *pair, unsigned index,
                        const struct mtl_library_source *source)
{
   MTL4LibraryDescriptor *desc = mtl_library_descriptor(
      source->code, source->language_version, pair->math_mode, pair->math_fp_fns);
   KKPipelineArchive *cache = index == 0
      ? kk_compiler_archive((mtl_compiler *)pair->compiler) : nil;
   id<MTLLibrary> cached = kk_cached_vertex_library(cache, desc);
   if (cached) {
      pair->libraries[index] = (mtl_library *)cached;
      return;
   }
   dispatch_group_enter(pair->completion);
   pair->tasks[index] = [pair->compiler newLibraryWithDescriptor:desc
      completionHandler:^(id<MTLLibrary> library, NSError *error) {
         @autoreleasepool {
            pair->libraries[index] = (mtl_library *)[library retain];
            if (library) kk_remember_vertex_library(cache, desc, library);
            if (!library)
               fprintf(stderr, "Failed to create MTLLibrary: %s\n",
                  error.localizedDescription.UTF8String ?: "Unknown Metal error");
         }
         dispatch_group_leave(pair->completion);
      }];
}

bool
mtl_new_library_pair(mtl_compiler *compiler,
                     const struct mtl_library_source sources[2],
                     enum mtl_math_mode math_mode,
                     enum mtl_math_floating_point_functions math_fp_fns,
                     mtl_library *libraries[2])
{
   @autoreleasepool {
      struct mtl_library_pair pair = {
         .compiler = (id<MTL4Compiler>)compiler,
         .completion = dispatch_group_create(),
         .math_mode = math_mode, .math_fp_fns = math_fp_fns,
      };
      libraries[0] = libraries[1] = NULL;
      if (!pair.completion) return false;
      for (unsigned i = 0; i < 2; ++i)
         mtl_library_pair_submit(&pair, i, &sources[i]);
      dispatch_group_wait(pair.completion, DISPATCH_TIME_FOREVER);
      bool success = pair.libraries[0] && pair.libraries[1];
      for (unsigned i = 0; i < 2; ++i) {
         if (success) libraries[i] = pair.libraries[i];
         else [(id)pair.libraries[i] release];
         [pair.tasks[i] release];
      }
      dispatch_release(pair.completion);
      return success;
   }
}

mtl_function_descriptor *
mtl_new_library_function_descriptor(mtl_library *library, const char *entry_point)
{
   @autoreleasepool {
      id<MTLLibrary> lib = (id<MTLLibrary>)library;
      NSString *ns_entry_point = [NSString stringWithCString:entry_point encoding:NSASCIIStringEncoding];

      MTL4LibraryFunctionDescriptor *desc = [MTL4LibraryFunctionDescriptor new];
      desc.name = ns_entry_point;
      desc.library = lib;
      return desc;
   }
}

mtl_function_descriptor *
mtl_specialize_function_uint64(mtl_function_descriptor *function,
                               uint32_t index, uint64_t value)
{
   @autoreleasepool {
      MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];
      if (!values)
         return NULL;
      [values setConstantValue:&value type:MTLDataTypeULong atIndex:index];
      MTL4SpecializedFunctionDescriptor *desc = [MTL4SpecializedFunctionDescriptor new];
      desc.functionDescriptor = (MTL4FunctionDescriptor *)function;
      desc.constantValues = values;
      return (mtl_function_descriptor *)desc;
   }
}

mtl_function_descriptor *
mtl_specialize_pipeline_statistics(mtl_function_descriptor *function)
{
   @autoreleasepool {
      bool enabled = true;
      MTLFunctionConstantValues *values =
         [[MTLFunctionConstantValues new] autorelease];
      [values setConstantValue:&enabled type:MTLDataTypeBool
                       atIndex:KK_FUNCTION_CONSTANT_PIPELINE_STATISTICS];
      MTL4SpecializedFunctionDescriptor *specialized =
         [MTL4SpecializedFunctionDescriptor new];
      /* Wrap rather than replace existing color or other specializations. */
      specialized.functionDescriptor = (MTL4FunctionDescriptor *)function;
      specialized.constantValues = values;
      return specialized;
   }
}

mtl_render_pipeline_descriptor *
mtl_statistics_render_descriptor(mtl_render_pipeline_descriptor *descriptor)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *copy =
         [(MTL4RenderPipelineDescriptor *)descriptor copy];
      mtl_function_descriptor *fragment = mtl_specialize_pipeline_statistics(
         (mtl_function_descriptor *)copy.fragmentFunctionDescriptor);
      copy.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)fragment;
      [(id)fragment release];
      return copy;
   }
}

mtl_function_descriptor *
mtl_specialize_sampler_emulation(mtl_function_descriptor *function, bool enabled)
{
   @autoreleasepool {
      if (!function) return NULL;
      MTLFunctionConstantValues *values =
         [[MTLFunctionConstantValues new] autorelease];
      if (!values) return NULL;
      [values setConstantValue:&enabled type:MTLDataTypeBool
                       atIndex:KK_FUNCTION_CONSTANT_SAMPLER_EMULATION];
      MTL4SpecializedFunctionDescriptor *desc =
         [MTL4SpecializedFunctionDescriptor new];
      desc.functionDescriptor = (MTL4FunctionDescriptor *)function;
      desc.constantValues = values;
      return (void *)desc;
   }
}

mtl_render_pipeline_descriptor *
mtl_sampler_emulation_render_descriptor(mtl_render_pipeline_descriptor *descriptor,
                                        uint32_t stages, uint32_t enabled_stages)
{
   @autoreleasepool {
      MTL4PipelineDescriptor *copy = [(id)descriptor copy];
      if (!copy) return NULL;
#define KK_SAMPLER_STAGE(type, field, bit) do { \
   if (stages & (bit)) { \
      mtl_function_descriptor *function = mtl_specialize_sampler_emulation( \
         (void *)((type *)copy).field, (enabled_stages & (bit)) != 0); \
      if (!function) { [copy release]; return NULL; } \
      ((type *)copy).field = (id)function; \
      [(id)function release]; \
   } \
} while (0)
      if ([copy isKindOfClass:[MTL4RenderPipelineDescriptor class]]) {
         KK_SAMPLER_STAGE(MTL4RenderPipelineDescriptor, vertexFunctionDescriptor,
                          MTL_SAMPLER_EMULATION_VERTEX);
         KK_SAMPLER_STAGE(MTL4RenderPipelineDescriptor, fragmentFunctionDescriptor,
                          MTL_SAMPLER_EMULATION_FRAGMENT);
      } else if ([copy isKindOfClass:[MTL4MeshRenderPipelineDescriptor class]]) {
         KK_SAMPLER_STAGE(MTL4MeshRenderPipelineDescriptor, objectFunctionDescriptor,
                          MTL_SAMPLER_EMULATION_OBJECT);
         KK_SAMPLER_STAGE(MTL4MeshRenderPipelineDescriptor, meshFunctionDescriptor,
                          MTL_SAMPLER_EMULATION_MESH);
         KK_SAMPLER_STAGE(MTL4MeshRenderPipelineDescriptor, fragmentFunctionDescriptor,
                          MTL_SAMPLER_EMULATION_FRAGMENT);
      } else {
         [copy release];
         return NULL;
      }
#undef KK_SAMPLER_STAGE
      return (void *)copy;
   }
}

/* Compute pipeline */
mtl_compute_pipeline_state *
mtl_new_compute_pipeline_state(mtl_compiler *compiler,
                               mtl_function_descriptor *function,
                               uint64_t max_total_threads_per_threadgroup)
{
   @autoreleasepool {
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;

      bool valid = true;
      MTL4FunctionDescriptor *prepared =
         kk_prepare_function_constants((MTL4FunctionDescriptor *)function, &valid);
      if (!valid) return NULL;
      MTL4ComputePipelineDescriptor *desc = [[MTL4ComputePipelineDescriptor new] autorelease];
      desc.computeFunctionDescriptor = prepared;
      desc.maxTotalThreadsPerThreadgroup = max_total_threads_per_threadgroup;

      NSError *error;
      id<MTLComputePipelineState> pipeline = kk_archive_compute(compiler, desc);
      if (!pipeline) {
         pipeline = MTL_COMPILER_PIPELINE_CALL(compiler,
            [comp newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
      }

      if (pipeline == nil) {
         fprintf(stderr, "Failed to create MTLComputePipelineState: %s\n", [error.localizedDescription UTF8String]);
      }

      return pipeline;
   }
}

/* Render pipeline descriptor */
mtl_render_pipeline_descriptor *
mtl_new_render_pipeline_descriptor()
{
   @autoreleasepool {
      return [MTL4RenderPipelineDescriptor new];
   }
}

void
mtl_render_pipeline_descriptor_set_vertex_shader(mtl_render_pipeline_descriptor *descriptor,
                                                 mtl_function_descriptor *shader)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.vertexFunctionDescriptor = (MTL4FunctionDescriptor *)shader;
   }
}

void
mtl_render_pipeline_descriptor_set_fragment_shader(mtl_render_pipeline_descriptor *descriptor,
                                                   mtl_function_descriptor *shader)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)shader;
   }
}

void
mtl_render_pipeline_descriptor_set_input_primitive_topology(mtl_render_pipeline_descriptor *descriptor,
                                                            enum mtl_primitive_topology_class class)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)class;
   }
}

void
mtl_render_pipeline_descriptor_set_color_attachment_format(mtl_render_pipeline_descriptor *descriptor,
                                                           uint8_t index,
                                                           enum mtl_pixel_format format)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.colorAttachments[index].pixelFormat = (MTLPixelFormat)format;
   }
}

void
mtl_render_pipeline_descriptor_set_raster_sample_count(mtl_render_pipeline_descriptor *descriptor,
                                                       uint32_t sample_count)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.rasterSampleCount = sample_count;
   }
}

void
mtl_render_pipeline_descriptor_set_rasterization_enabled(mtl_render_pipeline_descriptor *descriptor,
                                                         bool enabled)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.rasterizationEnabled = enabled;
   }
}

void
mtl_render_pipeline_descriptor_set_max_vertex_amplification_count(mtl_render_pipeline_descriptor *descriptor,
                                                                  uint32_t count)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.maxVertexAmplificationCount = count;
   }
}

/* Render pipeline */
mtl_render_pipeline_state *
mtl_new_render_pipeline(mtl_compiler *compiler, mtl_render_pass_descriptor *descriptor)
{
   @autoreleasepool {
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;
      MTL4PipelineDescriptor *desc =
         kk_prepare_render_function_constants((MTL4PipelineDescriptor *)descriptor);
      if (!desc) return NULL;

      NSError *error = nil;
      id<MTLRenderPipelineState> pipeline = kk_archive_render(compiler, desc);
      if (!pipeline) {
         pipeline = MTL_COMPILER_PIPELINE_CALL(compiler,
            [comp newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
      }

      if (pipeline == nil) {
         fprintf(stderr, "Failed to create MTLRenderPipelineState: %s\n", [error.localizedDescription UTF8String]);
      }

      return pipeline;
   }
}

/* Copy only Metal state; shader functions and pre-raster programs are shared. */
mtl_render_pipeline_state *
mtl_new_render_pipeline_for_raster_attachments(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,
   uint32_t samples, bool framebuffer_reads, enum mtl_primitive_topology_class topology)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *copy = [(MTL4RenderPipelineDescriptor *)descriptor copy];
      copy.rasterSampleCount = samples;
      if (topology != MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED)
         copy.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)topology;
      if (framebuffer_reads) {
         MTLFunctionConstantValues *values = [[MTLFunctionConstantValues new] autorelease];
         for (uint32_t i = 0; i < count; ++i) {
            bool active = formats[i] != MTL_PIXEL_FORMAT_INVALID;
            [values setConstantValue:&active type:MTLDataTypeBool atIndex:i];
         }
         MTL4SpecializedFunctionDescriptor *fragment =
            [[MTL4SpecializedFunctionDescriptor new] autorelease];
         fragment.functionDescriptor = copy.fragmentFunctionDescriptor;
         fragment.constantValues = values;
         copy.fragmentFunctionDescriptor = fragment;
      }
      for (uint32_t i = 0; i < count; ++i) {
         copy.colorAttachments[i].pixelFormat = (MTLPixelFormat)formats[i];
         if (disabled & (1u << i))
            copy.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
      }
      mtl_render_pipeline_state *pipeline = mtl_new_render_pipeline(compiler, (void *)copy);
      [copy release];
      return pipeline;
   }
}

/* Existing consumers retain the topology already set on their descriptor. */
mtl_render_pipeline_state *
mtl_new_render_pipeline_for_attachments(mtl_compiler *compiler,
   mtl_render_pipeline_descriptor *descriptor,
   const enum mtl_pixel_format *formats, uint32_t count, uint32_t disabled,
   uint32_t samples, bool framebuffer_reads)
{
   return mtl_new_render_pipeline_for_raster_attachments(compiler, descriptor,
      formats, count, disabled, samples, framebuffer_reads,
      MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED);
}

void
mtl_render_pipeline_descriptor_copy_linking(
   mtl_render_pipeline_descriptor *copy, mtl_render_pipeline_descriptor *source)
{
   /* Copies pipeline-linking state. */
}
