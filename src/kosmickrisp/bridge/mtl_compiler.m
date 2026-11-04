#include "kk_debug.h"
#include "util/log.h"
#include "util/mesa-blake3.h"
#include "mtl_gpu_timing.h"
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
#include <dispatch/dispatch.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

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

@interface KKCompileQueue : NSOperationQueue {
@public
   unsigned foreground, urgent, background_tasks;
}
@end
@implementation KKCompileQueue
@end

static char kk_compile_queue_association, kk_task_queue_association;
static unsigned kk_background_pending;
static _Thread_local bool kk_background_thread;
static _Thread_local NSOperation *kk_current_operation;
static pthread_mutex_t kk_compile_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kk_compile_queue_changed = PTHREAD_COND_INITIALIZER;

/* Cancelled queue objects may finish their Foundation bookkeeping after the
 * Vulkan payload is retired. Pin this callback image only on first KK-worker
 * use, so loader dlclose cannot unload those methods. No Metal callback exists. */
static bool
kk_pin_worker_image(void)
{
   static dispatch_once_t once;
   static bool ready;
   dispatch_once(&once, ^{
      Dl_info image;
      if (!dladdr((const void *)kk_pin_worker_image, &image)) return;
      if (image.dli_fbase == (const void *)_dyld_get_image_header(0)) ready = true;
      else ready = dlopen(image.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != NULL;
   });
   return ready;
}


/* KK owns the worker and calls Metal synchronously. Never hand compile work to
 * Metal completion handlers inside a Wine process. Foundation owns only the
 * operation state; cancellation retires an unentered C payload exactly once. */
@interface KKCompileOperation : NSOperation {
@public
   void *data;
   void (*execute)(void *), (*failed)(void *, const char *), (*cleanup)(void *);
   KKCompileQueue *ownerQueue;
   pthread_mutex_t state;
   bool callback_entered, payload_claimed, payload_retired, frontend;
}
- (void)retirePayload:(BOOL)onlyQueued;
- (BOOL)payloadRetired;
@end
@implementation KKCompileOperation
- (id)init
{
   self = [super init];
   if (self && pthread_mutex_init(&state, NULL)) { [super dealloc]; return nil; }
   return self;
}
- (BOOL)payloadRetired { return __atomic_load_n(&payload_retired, __ATOMIC_ACQUIRE); }
- (void)retirePayload:(BOOL)onlyQueued
{
   pthread_mutex_lock(&state);
   if (payload_claimed || (onlyQueued && callback_entered)) {
      pthread_mutex_unlock(&state);
      return;
   }
   payload_claimed = true;
   void *payload = data;
   void (*finish_payload)(void *) = cleanup;
   data = NULL; execute = NULL; failed = NULL; cleanup = NULL;
   pthread_mutex_unlock(&state);
   if (frontend) {
      pthread_mutex_lock(&kk_compile_queue_lock);
      assert(ownerQueue->urgent);
      --ownerQueue->urgent;
      if (ownerQueue->foreground && !ownerQueue->urgent) ownerQueue.suspended = YES;
      pthread_cond_broadcast(&kk_compile_queue_changed);
      pthread_mutex_unlock(&kk_compile_queue_lock);
   } else {
      pthread_mutex_lock(&kk_compile_queue_lock);
      assert(ownerQueue->background_tasks);
      --ownerQueue->background_tasks;
      pthread_mutex_unlock(&kk_compile_queue_lock);
      __atomic_sub_fetch(&kk_background_pending, 1, __ATOMIC_RELEASE);
   }
   finish_payload(payload);
   __atomic_store_n(&payload_retired, true, __ATOMIC_RELEASE);
}
- (void)cancel
{
   [super cancel];
   [self retirePayload:YES];
   pthread_mutex_lock(&kk_compile_queue_lock);
   pthread_cond_broadcast(&kk_compile_queue_changed);
   pthread_mutex_unlock(&kk_compile_queue_lock);
}
- (void)main
{
   @autoreleasepool {
      pthread_mutex_lock(&kk_compile_queue_lock);
      while (!frontend && ownerQueue->foreground && !ownerQueue->urgent && !self.cancelled)
         pthread_cond_wait(&kk_compile_queue_changed, &kk_compile_queue_lock);
      pthread_mutex_unlock(&kk_compile_queue_lock);
      pthread_mutex_lock(&state);
      bool retired = payload_claimed;
      if (!retired) callback_entered = true;
      pthread_mutex_unlock(&state);
      if (retired) return;
      kk_background_thread = true;
      kk_current_operation = self;
      @try { if (!self.cancelled) execute(data); }
      @catch (NSException *exception) { failed(data, exception.reason.UTF8String); }
      @finally {
         kk_current_operation = nil;
         kk_background_thread = false;
         [self retirePayload:NO];
      }
   }
}
- (void)dealloc
{
   [ownerQueue release];
   pthread_mutex_destroy(&state);
   [super dealloc];
}
@end

/* A zero pending count avoids Objective-C lookup, locks and queue changes on
 * ordinary synchronous compiles. Background callers never pause their queue. */
void *
mtl_compiler_foreground_begin(mtl_compiler *compiler)
{
   if (kk_background_thread || !__atomic_load_n(&kk_background_pending, __ATOMIC_ACQUIRE))
      return NULL;
   pthread_mutex_lock(&kk_compile_queue_lock);
   KKCompileQueue *queue = objc_getAssociatedObject((id)compiler, &kk_compile_queue_association);
   if (queue && queue.operationCount) {
      ++queue->foreground;
      if (!queue->urgent) queue.suspended = YES;
   } else queue = nil;
   pthread_mutex_unlock(&kk_compile_queue_lock);
   return queue;
}

void
mtl_compiler_foreground_end(void *token)
{
   if (!token) return;
   KKCompileQueue *queue = (KKCompileQueue *)token;
   pthread_mutex_lock(&kk_compile_queue_lock);
   assert(queue->foreground);
   if (!--queue->foreground) queue.suspended = NO;
   pthread_cond_broadcast(&kk_compile_queue_changed);
   pthread_mutex_unlock(&kk_compile_queue_lock);
}

static void
kk_foreground_cleanup(void **token)
{
   mtl_compiler_foreground_end(*token);
}
#define KK_FOREGROUND_SCOPE(compiler) \
   void *foreground __attribute__((cleanup(kk_foreground_cleanup))) = \
      mtl_compiler_foreground_begin(compiler)

/* Native miss scheduling only: cached deserialization stays on its original
 * synchronous path. Never retain caller create-info or feedback pointers. */
static void *
kk_compiler_submit(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *), bool frontend)
{
   @autoreleasepool {
      pthread_mutex_lock(&kk_compile_queue_lock);
      KKCompileQueue *queue = objc_getAssociatedObject((id)compiler, &kk_compile_queue_association);
      if (!queue) {
         if (!kk_pin_worker_image()) { pthread_mutex_unlock(&kk_compile_queue_lock); return NULL; }
         queue = [[KKCompileQueue alloc] init];
         if (queue) {
            NSUInteger cores = MIN((NSUInteger)8, [NSProcessInfo processInfo].activeProcessorCount);
            queue.maxConcurrentOperationCount = cores > 1 ? cores - 1 : 1;
            queue.qualityOfService = NSQualityOfServiceUtility;
            queue.name = @"KosmicKrisp compiler";
            objc_setAssociatedObject((id)compiler, &kk_compile_queue_association, queue, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [queue release];
         }
      }
      /* A pair never queues behind unrelated jobs. Its caller can compile both
       * stages synchronously when this same bounded queue is already occupied. */
      if (!queue || (frontend && (queue->background_tasks || queue.operationCount >= queue.maxConcurrentOperationCount))) {
         pthread_mutex_unlock(&kk_compile_queue_lock);
         return NULL;
      }
      KKCompileOperation *operation = [KKCompileOperation new];
      if (!operation) { pthread_mutex_unlock(&kk_compile_queue_lock); return NULL; }
      operation->data = data;
      operation->execute = execute;
      operation->failed = failed;
      operation->cleanup = cleanup;
      operation->ownerQueue = [queue retain];
      operation->frontend = frontend;
      operation.qualityOfService = frontend ? NSQualityOfServiceUserInitiated : NSQualityOfServiceUtility;
      if (frontend) {
         operation.queuePriority = NSOperationQueuePriorityVeryHigh;
         ++queue->urgent;
      } else {
         ++queue->background_tasks;
         __atomic_add_fetch(&kk_background_pending, 1, __ATOMIC_RELEASE);
      }
      objc_setAssociatedObject(operation, &kk_task_queue_association, queue, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      [queue addOperation:operation];
      if (frontend) queue.suspended = NO;
      pthread_mutex_unlock(&kk_compile_queue_lock);
      return operation;
   }
}

void *
mtl_compiler_submit(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *))
{
   return kk_compiler_submit(compiler, data, execute, failed, cleanup, false);
}

void
mtl_compiler_task_cancel(void *task)
{
   [(NSOperation *)task cancel];
}

void
mtl_compiler_task_wait(void *task)
{
   if (!task) return;
   KKCompileOperation *operation = (KKCompileOperation *)task;
   if (operation.payloadRetired) return;
   if (!operation.finished) {
      /* Wait only for this pipeline. Promote a queued first-use request over
       * unrelated cold misses; running native jobs remain machine-bounded. */
      operation.queuePriority = NSOperationQueuePriorityVeryHigh;
      KKCompileQueue *queue = objc_getAssociatedObject(operation, &kk_task_queue_association);
      if (queue) {
         pthread_mutex_lock(&kk_compile_queue_lock);
         ++queue->urgent;
         /* A selected first-use task is foreground work. It must not wait for
          * an unrelated caller compile just because admission is suspended. */
         queue.suspended = NO;
         pthread_cond_broadcast(&kk_compile_queue_changed);
         pthread_mutex_unlock(&kk_compile_queue_lock);
      }
      [operation waitUntilFinished];
      if (queue) {
         pthread_mutex_lock(&kk_compile_queue_lock);
         --queue->urgent;
         if (queue->foreground && !queue->urgent) queue.suspended = YES;
         pthread_cond_broadcast(&kk_compile_queue_changed);
         pthread_mutex_unlock(&kk_compile_queue_lock);
      }
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
   if (KK_DEBUG(MSL)) {
      blake3_hash hash; char hex[BLAKE3_HEX_LEN];
      _mesa_blake3_compute(src, strlen(src), hash); _mesa_blake3_format(hex, hash);
      mesa_logi("// KK_MSL blake3=%s stage=native_library bytes=%zu\n%s\n", hex, strlen(src), src);
   }
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
      KK_FOREGROUND_SCOPE(compiler);
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;
      MTL4LibraryDescriptor *desc =
         mtl_library_descriptor(src, msl_version, math_mode, math_fp_fns);
      NSError *error = nil;
      id<MTLLibrary> lib = [comp newLibraryWithDescriptor:desc error:&error];
      if (lib == nil)
         fprintf(stderr, "Failed to create MTLLibrary: %s\n",
                 error.localizedDescription.UTF8String ?: "Unknown Metal error");
      if (__atomic_load_n(&mtl_gpu_timing_enabled, __ATOMIC_ACQUIRE))
         mtl_gpu_timing_library(lib, src);
      return lib;
   }
}

/* Historical bridge name: scheduling is asynchronous only at the KK queue.
 * The Metal call itself MUST be synchronous in Wine on either CPU. */
mtl_library *
mtl_new_library_async_wait(mtl_compiler *compiler, const char *src,
   enum mtl_language_version version, enum mtl_math_mode mode,
   enum mtl_math_floating_point_functions fp)
{
   if (kk_current_operation.cancelled) return NULL;
   mtl_library *library = mtl_new_library(compiler, src, version, mode, fp);
   if (kk_current_operation.cancelled) {
      [(id)library release];
      return NULL;
   }
   return library;
}

struct kk_library_work {
   mtl_compiler *compiler;
   MTL4LibraryDescriptor *descriptor;
   mtl_library *result;
   bool vertex;
};

static void
kk_library_work_execute(void *data)
{
   struct kk_library_work *work = data;
   work->result = kk_graphics_library(work->compiler, work->descriptor, work->vertex, false);
   if (__atomic_load_n(&mtl_gpu_timing_enabled, __ATOMIC_ACQUIRE)) {
      NSString *source = work->descriptor.source;
      if (debug_msl_prefix && [source hasPrefix:debug_msl_prefix])
         source = [source substringFromIndex:debug_msl_prefix.length];
      mtl_gpu_timing_library(work->result, source.UTF8String);
   }
}

static void
kk_library_work_failed(void *data, const char *reason)
{
   (void)data;
   fprintf(stderr, "Metal library compilation exception: %s\n", reason ?: "Unknown Metal exception");
}

static void
kk_library_work_cleanup(void *data)
{
   (void)data; /* The pair's stack remains alive until task_wait returns. */
}

bool
mtl_new_library_pair(mtl_compiler *compiler,
                     const struct mtl_library_source sources[2],
                     enum mtl_math_mode math_mode,
                     enum mtl_math_floating_point_functions math_fp_fns,
                     mtl_library *libraries[2])
{
   @autoreleasepool {
      KK_FOREGROUND_SCOPE(compiler);
      libraries[0] = libraries[1] = NULL;
      struct kk_library_work work[2] = {
         {compiler, mtl_library_descriptor(sources[0].code, sources[0].language_version, math_mode, math_fp_fns), NULL, true},
         {compiler, mtl_library_descriptor(sources[1].code, sources[1].language_version, math_mode, math_fp_fns), NULL, false},
      };
      /* Completed hits need no worker or queue admission. Pending requests
       * join only on entry to execute, preserving independent stage overlap. */
      for (unsigned i = 0; i < 2; ++i)
         work[i].result = kk_graphics_library(compiler, work[i].descriptor, work[i].vertex, true);
      void *task = NULL;
      @try {
         if (!work[0].result && !kk_background_thread)
            task = kk_compiler_submit(compiler, &work[0], kk_library_work_execute,
               kk_library_work_failed, kk_library_work_cleanup, true);
         if (!task && !work[0].result) kk_library_work_execute(&work[0]);
         if (!work[1].result) kk_library_work_execute(&work[1]);
      } @catch (NSException *exception) {
         kk_library_work_failed(NULL, exception.reason.UTF8String);
      } @finally {
         mtl_compiler_task_wait(task);
         [(id)task release];
      }
      bool ready = work[0].result && work[1].result;
      for (unsigned i = 0; i < 2; ++i) {
         if (ready) libraries[i] = work[i].result;
         else [(id)work[i].result release];
      }
      return ready;
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
      MTL4SpecializedFunctionDescriptor *desc =
         kk_new_function_specialization((id)function);
      [desc.constantValues setConstantValue:&value type:MTLDataTypeULong atIndex:index];
      return (mtl_function_descriptor *)desc;
   }
}

mtl_function_descriptor *
mtl_specialize_pipeline_statistics(mtl_function_descriptor *function)
{
   @autoreleasepool {
      bool enabled = true;
      MTL4SpecializedFunctionDescriptor *specialized =
         kk_new_function_specialization((id)function);
      [specialized.constantValues setConstantValue:&enabled type:MTLDataTypeBool
                       atIndex:KK_FUNCTION_CONSTANT_PIPELINE_STATISTICS];
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
      if (!fragment) { [copy release]; return NULL; }
      copy.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)fragment;
      [(id)fragment release];
      return copy;
   }
}

mtl_function_descriptor *
mtl_specialize_sampler_emulation(mtl_function_descriptor *function, bool enabled)
{
   @autoreleasepool {
      MTL4SpecializedFunctionDescriptor *desc =
         kk_new_function_specialization((id)function);
      [desc.constantValues setConstantValue:&enabled type:MTLDataTypeBool
                       atIndex:KK_FUNCTION_CONSTANT_SAMPLER_EMULATION];
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
      KK_FOREGROUND_SCOPE(compiler);
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;

      bool valid = true;
      MTL4FunctionDescriptor *prepared =
         kk_prepare_function_constants((MTL4FunctionDescriptor *)function, &valid);
      if (!valid) return NULL;
      MTL4ComputePipelineDescriptor *desc = [[MTL4ComputePipelineDescriptor new] autorelease];
      desc.computeFunctionDescriptor = prepared;
      desc.maxTotalThreadsPerThreadgroup = max_total_threads_per_threadgroup;

      if (kk_current_operation.cancelled) return NULL;
      NSError *error = nil;
      id<MTLComputePipelineState> pipeline = kk_archive_compute(compiler, desc);
      if (!pipeline) {
         pipeline = MTL_COMPILER_PIPELINE_CALL(compiler,
            [comp newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
      }

      if (kk_current_operation.cancelled) {
         [pipeline release];
         return NULL;
      }
      if (pipeline == nil) {
         fprintf(stderr, "Failed to create MTLComputePipelineState: %s\n", [error.localizedDescription UTF8String]);
      }

      if (__atomic_load_n(&mtl_gpu_timing_enabled, __ATOMIC_ACQUIRE))
         mtl_gpu_timing_pipeline(pipeline, desc, false);
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
      KK_FOREGROUND_SCOPE(compiler);
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
         MTL4SpecializedFunctionDescriptor *fragment =
            kk_new_function_specialization(copy.fragmentFunctionDescriptor);
         if (!fragment) { [copy release]; return NULL; }
         for (uint32_t i = 0; i < count; ++i) {
            bool active = formats[i] != MTL_PIXEL_FORMAT_INVALID;
            [fragment.constantValues setConstantValue:&active type:MTLDataTypeBool atIndex:i];
         }
         copy.fragmentFunctionDescriptor = fragment;
         [fragment release];
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
