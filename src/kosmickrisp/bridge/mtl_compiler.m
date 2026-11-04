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

struct mtl_compile_trace
mtl_compiler_set_trace(struct mtl_compile_trace trace)
{
   struct mtl_compile_trace previous = kk_compile_trace;
   kk_compile_trace = trace;
   return previous;
}

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

/* Bypassed pipelines captured per compile on the calling thread. A capture is
 * served by Metal's compiled-binary cache, so a batch costs well under 1 ms. */
#define KK_RECAPTURE_BATCH 4

void
mtl_compiler_finish_cache(mtl_compiler *compiler)
{
   @autoreleasepool {
      mtl_compiler_cache_recapture(compiler, UINT_MAX);
      [kk_compiler_archive(compiler) finish];
   }
}

@interface KKCompileQueue : NSOperationQueue {
@public
   unsigned foreground, urgent, background_tasks;
}
@end
@implementation KKCompileQueue
@end

static char kk_compile_queue_association, kk_task_queue_association;
static char kk_predicted_queue_association, kk_stage_queue_association;
static unsigned kk_background_pending;
/* Last start or end of a demand (non-background) Metal compile, from
 * kk_archive_now(). Deferred builds start only after this much quiet:
 * a background-QoS compile in flight slowed the same foreground compile
 * load by about a third while finishing almost nothing itself. */
static uint64_t kk_foreground_mark;
#ifndef KK_BACKGROUND_QUIET_NS
#define KK_BACKGROUND_QUIET_NS 500000000ull
#endif
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
   pthread_cond_t retired;
   pthread_t worker;
   bool callback_entered, payload_claimed, payload_retired, frontend, predicted;
}
- (void)retirePayload:(BOOL)onlyQueued;
- (BOOL)payloadRetired;
@end
@implementation KKCompileOperation
- (id)init
{
   self = [super init];
   if (self && pthread_mutex_init(&state, NULL)) { [super dealloc]; return nil; }
   if (self && pthread_cond_init(&retired, NULL)) {
      pthread_mutex_destroy(&state);
      [super dealloc];
      return nil;
   }
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
   pthread_mutex_lock(&state);
   __atomic_store_n(&payload_retired, true, __ATOMIC_RELEASE);
   pthread_cond_broadcast(&retired);
   pthread_mutex_unlock(&state);
}
- (void)waitRetired
{
   pthread_mutex_lock(&state);
   while (!payload_retired) pthread_cond_wait(&retired, &state);
   pthread_mutex_unlock(&state);
}
- (void)run
{
   bool background = kk_background_thread;
   NSOperation *previous = kk_current_operation;
   kk_background_thread = true;
   kk_current_operation = self;
   @try { if (!self.cancelled) execute(data); }
   @catch (NSException *exception) { failed(data, exception.reason.UTF8String); }
   @finally {
      kk_current_operation = previous;
      kk_background_thread = background;
      [self retirePayload:NO];
   }
}
/* A waiter runs a payload that no worker has entered, at its own priority,
 * instead of waiting for a queue slot. The worker that later starts this
 * operation finds the payload entered and returns. */
- (BOOL)runHere
{
   pthread_mutex_lock(&state);
   bool claim = !payload_claimed && !callback_entered;
   if (claim) callback_entered = true;
   pthread_mutex_unlock(&state);
   if (!claim) return NO;
   [self retain];
   @autoreleasepool { [self run]; }
   [self release];
   return YES;
}
/* Raise the worker compiling this payload. Retirement takes the state lock,
 * so the worker is alive while the override starts. */
- (pthread_override_t)raiseWorker
{
   pthread_mutex_lock(&state);
   pthread_override_t raised = callback_entered && !payload_claimed && worker ?
      kk_raise_background_compile(worker) : NULL;
   pthread_mutex_unlock(&state);
   return raised;
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
      /* A prestarted or joined payload already ran elsewhere. */
      pthread_mutex_lock(&state);
      bool taken = payload_claimed || callback_entered;
      pthread_mutex_unlock(&state);
      if (taken) return;
      pthread_mutex_lock(&kk_compile_queue_lock);
      /* A deferred build waits while demand compiles run and until they have
       * been quiet for KK_BACKGROUND_QUIET_NS. A waiter needing a build runs
       * it itself or marks the queue urgent; cancellation also wakes it. A
       * predicted build starts at once on its own queue. */
      while (!frontend && !predicted && !ownerQueue->urgent && !self.cancelled) {
         if (ownerQueue->foreground) {
            pthread_cond_wait(&kk_compile_queue_changed, &kk_compile_queue_lock);
            continue;
         }
         uint64_t mark = __atomic_load_n(&kk_foreground_mark, __ATOMIC_ACQUIRE);
         uint64_t now = kk_archive_now();
         if (!mark || now < mark || now - mark >= KK_BACKGROUND_QUIET_NS) break;
         uint64_t left = KK_BACKGROUND_QUIET_NS - (now - mark);
         struct timespec wait = { (time_t)(left / 1000000000ull), (long)(left % 1000000000ull) };
         pthread_cond_timedwait_relative_np(&kk_compile_queue_changed, &kk_compile_queue_lock, &wait);
      }
      pthread_mutex_unlock(&kk_compile_queue_lock);
      pthread_mutex_lock(&state);
      bool skip = payload_claimed || callback_entered;
      if (!skip) {
         callback_entered = true;
         if (!frontend) worker = pthread_self();
      }
      pthread_mutex_unlock(&state);
      if (!skip) [self run];
   }
}
- (void)dealloc
{
   [ownerQueue release];
   pthread_cond_destroy(&retired);
   pthread_mutex_destroy(&state);
   [super dealloc];
}
@end

/* A zero pending count avoids Objective-C lookup, locks and queue changes on
 * ordinary synchronous compiles. Background callers never pause their queue. */
void *
mtl_compiler_foreground_begin(mtl_compiler *compiler)
{
   if (kk_background_thread) return NULL;
   __atomic_store_n(&kk_foreground_mark, kk_archive_now(), __ATOMIC_RELEASE);
   if (!__atomic_load_n(&kk_background_pending, __ATOMIC_ACQUIRE))
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
   if (!kk_background_thread)
      __atomic_store_n(&kk_foreground_mark, kk_archive_now(), __ATOMIC_RELEASE);
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
/* Predicted builds (pipelines the application bound in an earlier run) use a
 * second queue of two utility workers that demand compiles never suspend, so
 * they are ready before their first bind instead of compiling on the binding
 * thread. */
enum kk_queue_kind { KK_QUEUE_DEFERRED, KK_QUEUE_PREDICTED, KK_QUEUE_STAGE };

/* Deferred builds share one background queue that demand compiles pause.
 * Predicted builds use two utility workers that are never paused. The second
 * stage of a demand library pair uses its own user-initiated queue, so the
 * two stages of a pipeline compile in parallel even while deferred builds
 * are queued. Called with kk_compile_queue_lock held. */
static KKCompileQueue *
kk_compile_queue(mtl_compiler *compiler, enum kk_queue_kind kind)
{
   void *association = kind == KK_QUEUE_PREDICTED ? &kk_predicted_queue_association :
      kind == KK_QUEUE_STAGE ? &kk_stage_queue_association : &kk_compile_queue_association;
   KKCompileQueue *queue = objc_getAssociatedObject((id)compiler, association);
   if (queue || !kk_pin_worker_image()) return queue;
   queue = [[KKCompileQueue alloc] init];
   if (!queue) return nil;
   NSUInteger cores = MIN((NSUInteger)8, [NSProcessInfo processInfo].activeProcessorCount);
   queue.maxConcurrentOperationCount = kind == KK_QUEUE_PREDICTED ? MIN((NSUInteger)2, cores) :
      kind == KK_QUEUE_STAGE ? cores : cores > 1 ? cores - 1 : 1;
   /* A queue's service level is also the floor for its operations. */
   queue.qualityOfService = kind == KK_QUEUE_PREDICTED ? NSQualityOfServiceUtility :
      kind == KK_QUEUE_STAGE ? NSQualityOfServiceUserInitiated : NSQualityOfServiceBackground;
   queue.name = kind == KK_QUEUE_PREDICTED ? @"KosmicKrisp predicted builds" :
      kind == KK_QUEUE_STAGE ? @"KosmicKrisp pipeline stages" : @"KosmicKrisp compiler";
   objc_setAssociatedObject((id)compiler, association, queue, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   [queue release];
   return queue;
}

static void *
kk_compiler_submit(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *), bool frontend, bool predicted)
{
   @autoreleasepool {
      pthread_mutex_lock(&kk_compile_queue_lock);
      KKCompileQueue *queue = kk_compile_queue(compiler, frontend ? KK_QUEUE_STAGE :
         predicted ? KK_QUEUE_PREDICTED : KK_QUEUE_DEFERRED);
      /* A pair stage never waits for a worker: its caller compiles both stages
       * when every stage worker is busy. */
      if (!queue || (frontend && queue.operationCount >= queue.maxConcurrentOperationCount)) {
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
      operation->predicted = predicted;
      /* Deferred builds yield the CPU and the compiler service to the
       * application; a waiter runs or raises the one it needs. */
      operation.qualityOfService = frontend ? NSQualityOfServiceUserInitiated :
         predicted ? NSQualityOfServiceUtility : NSQualityOfServiceBackground;
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
   return kk_compiler_submit(compiler, data, execute, failed, cleanup, false, false);
}

void *
mtl_compiler_submit_predicted(mtl_compiler *compiler, void *data,
   void (*execute)(void *), void (*failed)(void *, const char *),
   void (*cleanup)(void *))
{
   return kk_compiler_submit(compiler, data, execute, failed, cleanup, false, true);
}

/* A deferred build queued before it was known to be predicted runs on a
 * predicted-build worker if no worker or waiter has entered it yet. */
void
mtl_compiler_task_prestart(mtl_compiler *compiler, void *task)
{
   @autoreleasepool {
      KKCompileOperation *operation = (KKCompileOperation *)task;
      if (!operation || operation->frontend || operation.payloadRetired) return;
      pthread_mutex_lock(&kk_compile_queue_lock);
      KKCompileQueue *queue = kk_compile_queue(compiler, KK_QUEUE_PREDICTED);
      pthread_mutex_unlock(&kk_compile_queue_lock);
      if (!queue) return;
      [operation retain];
      NSBlockOperation *runner = [NSBlockOperation blockOperationWithBlock:^{
         [operation runHere];
         [operation release];
      }];
      runner.qualityOfService = NSQualityOfServiceUtility;
      [queue addOperation:runner];
   }
}

void
mtl_compiler_task_cancel(void *task)
{
   [(NSOperation *)task cancel];
}

enum mtl_compiler_join
mtl_compiler_task_join(void *task)
{
   if (!task) return MTL_COMPILER_JOIN_NONE;
   KKCompileOperation *operation = (KKCompileOperation *)task;
   if (operation.payloadRetired) return MTL_COMPILER_JOIN_NONE;
   if (!operation->frontend && [operation runHere]) return MTL_COMPILER_JOIN_HERE;
   enum mtl_compiler_join join = MTL_COMPILER_JOIN_QUEUED;
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
      pthread_override_t raised = operation->frontend ? NULL : [operation raiseWorker];
      if (raised) join = MTL_COMPILER_JOIN_RUNNING;
      [operation waitUntilFinished];
      if (raised) pthread_override_qos_class_end_np(raised);
      if (queue) {
         pthread_mutex_lock(&kk_compile_queue_lock);
         --queue->urgent;
         if (queue->foreground && !queue->urgent) queue.suspended = YES;
         pthread_cond_broadcast(&kk_compile_queue_changed);
         pthread_mutex_unlock(&kk_compile_queue_lock);
      }
   }
   /* A payload run by a waiter can outlast its finished operation. */
   [operation waitRetired];
   return join;
}

void
mtl_compiler_task_wait(void *task)
{
   mtl_compiler_task_join(task);
}

void *
mtl_compiler_task_raise(void *task)
{
   KKCompileOperation *operation = (KKCompileOperation *)task;
   return operation && !operation->frontend ? [operation raiseWorker] : NULL;
}

void
mtl_compiler_task_raise_end(void *raised)
{
   if (raised) pthread_override_qos_class_end_np(raised);
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
      mesa_logi("// KK_MSL_LIBRARY blake3=%s stage=native_library bytes=%zu\n%s\n", hex, strlen(src), src);
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
      MTL4LibraryDescriptor *desc =
         mtl_library_descriptor(src, msl_version, math_mode, math_fp_fns);
      id<MTLLibrary> lib = kk_cached_library(compiler, desc, false, false);
      if (__atomic_load_n(&mtl_gpu_timing_enabled, __ATOMIC_ACQUIRE))
         mtl_gpu_timing_library(lib, src);
      return lib;
   }
}

mtl_library *
mtl_new_linkable_library(mtl_compiler *compiler, const char *src,
   enum mtl_language_version version, enum mtl_math_mode mode,
   enum mtl_math_floating_point_functions fp)
{
   @autoreleasepool {
      KK_FOREGROUND_SCOPE(compiler);
      MTL4LibraryDescriptor *desc = mtl_library_descriptor(src, version, mode, fp);
      desc.options.allowReferencingUndefinedSymbols = YES;
      id<MTLLibrary> lib = kk_cached_library(compiler, desc, false, false);
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
   struct mtl_compile_trace trace;
};

static void
kk_library_work_execute(void *data)
{
   struct kk_library_work *work = data;
   struct mtl_compile_trace previous = {0};
   if (work->trace.event) previous = mtl_compiler_set_trace(work->trace);
   @try {
      work->result = kk_cached_library(work->compiler, work->descriptor, work->vertex, false);
   } @finally {
      if (work->trace.event) mtl_compiler_set_trace(previous);
   }
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
      for (unsigned i = 0; i < 2; ++i) {
         work[i].trace = kk_compile_trace;
         work[i].result = kk_cached_library(compiler, work[i].descriptor, work[i].vertex, true);
      }
      void *task = NULL;
      @try {
         if (!work[0].result && !kk_background_thread)
            task = kk_compiler_submit(compiler, &work[0], kk_library_work_execute,
               kk_library_work_failed, kk_library_work_cleanup, true, false);
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
      kk_set_function_constant(desc, &value, MTLDataTypeULong, sizeof(value), index);
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
      kk_set_function_constant(specialized, &enabled, MTLDataTypeBool, sizeof(enabled),
                               KK_FUNCTION_CONSTANT_PIPELINE_STATISTICS);
      return specialized;
   }
}

/* Link helpers have their own constant-value snapshots. Propagate a stage
 * specialization only to functions that actually declare that constant. */
static NSArray *
kk_specialized_link_functions(NSArray<MTL4FunctionDescriptor *> *functions,
                              uint32_t index, bool enabled)
{
   NSMutableArray *result = [NSMutableArray arrayWithCapacity:functions.count];
   for (MTL4FunctionDescriptor *original in functions) {
      MTL4FunctionDescriptor *raw = original;
      if ([raw isKindOfClass:[MTL4SpecializedFunctionDescriptor class]])
         raw = ((MTL4SpecializedFunctionDescriptor *)raw).functionDescriptor;
      if (!kk_named_library_function(raw)) return nil;
      MTL4LibraryFunctionDescriptor *source = (id)raw;
      id<MTLFunction> function = [source.library newFunctionWithName:source.name];
      if (!function) return nil;
      bool uses = false;
      for (MTLFunctionConstant *constant in function.functionConstantsDictionary.allValues)
         uses |= constant.index == index && constant.type == MTLDataTypeBool;
      [function release];
      if (!uses) {
         [result addObject:original];
         continue;
      }
      MTL4SpecializedFunctionDescriptor *copy = kk_new_function_specialization(original);
      if (!copy) return nil;
      kk_set_function_constant(copy, &enabled, MTLDataTypeBool, sizeof(enabled), index);
      [result addObject:copy];
      [copy release];
   }
   return result;
}

static bool
kk_specialize_static_link(MTL4StaticLinkingDescriptor *original, uint32_t index,
                          bool enabled, MTL4StaticLinkingDescriptor **output)
{
   *output = nil;
   if (!original.functionDescriptors.count && !original.privateFunctionDescriptors.count &&
       !original.groups.count) return true;
   NSArray *functions = kk_specialized_link_functions(original.functionDescriptors, index, enabled);
   NSArray *private_functions = kk_specialized_link_functions(original.privateFunctionDescriptors, index, enabled);
   if (!functions || !private_functions) return false;
   NSMutableDictionary *groups = [NSMutableDictionary dictionary];
   for (NSString *key in original.groups) {
      NSArray *members = kk_specialized_link_functions(original.groups[key], index, enabled);
      if (!members) return false;
      groups[key] = members;
   }
   MTL4StaticLinkingDescriptor *copy = [[original copy] autorelease];
   if (!copy) return false;
   copy.functionDescriptors = functions;
   copy.privateFunctionDescriptors = private_functions;
   copy.groups = groups;
   *output = copy;
   return true;
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
      kk_carry_constant_record(copy, (id)descriptor);
      copy.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)fragment;
      kk_record_stage_constants(copy, @"fragmentFunctionDescriptor", (id)fragment, true);
      [(id)fragment release];
      MTL4StaticLinkingDescriptor *link = nil;
      if (!kk_specialize_static_link(copy.fragmentStaticLinkingDescriptor,
             KK_FUNCTION_CONSTANT_PIPELINE_STATISTICS, true, &link)) {
         [copy release]; return NULL;
      }
      if (link) copy.fragmentStaticLinkingDescriptor = link;
      return copy;
   }
}

mtl_function_descriptor *
mtl_specialize_sampler_emulation(mtl_function_descriptor *function, uint32_t enabled)
{
   @autoreleasepool {
      MTL4SpecializedFunctionDescriptor *desc =
         kk_new_function_specialization((id)function);
      bool generic = enabled & KK_SAMPLER_EMULATION_GENERIC;
      bool cube = enabled & KK_SAMPLER_EMULATION_CUBE;
      kk_set_function_constant(desc, &generic, MTLDataTypeBool, sizeof(generic),
                               KK_FUNCTION_CONSTANT_SAMPLER_EMULATION);
      kk_set_function_constant(desc, &cube, MTLDataTypeBool, sizeof(cube),
                               KK_FUNCTION_CONSTANT_NON_SEAMLESS_CUBE);
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
      kk_carry_constant_record(copy, (id)descriptor);
#define KK_SAMPLER_STAGE(type, field, linking, bit) do { \
   if (stages & (bit)) { \
      mtl_function_descriptor *function = mtl_specialize_sampler_emulation( \
         (void *)((type *)copy).field, \
         ((enabled_stages & (bit)) ? KK_SAMPLER_EMULATION_GENERIC : 0) | \
         ((enabled_stages & ((bit) << 4)) ? KK_SAMPLER_EMULATION_CUBE : 0)); \
      if (!function) { [copy release]; return NULL; } \
      ((type *)copy).field = (id)function; \
      kk_record_stage_constants(copy, @#field, (id)function, true); \
      [(id)function release]; \
      MTL4StaticLinkingDescriptor *link = nil; \
      if (!kk_specialize_static_link(((type *)copy).linking, \
            KK_FUNCTION_CONSTANT_SAMPLER_EMULATION, (enabled_stages & (bit)) != 0, &link)) { \
         [copy release]; return NULL; \
      } \
      if (link) ((type *)copy).linking = link; \
   } \
} while (0)
      if ([copy isKindOfClass:[MTL4RenderPipelineDescriptor class]]) {
         KK_SAMPLER_STAGE(MTL4RenderPipelineDescriptor, vertexFunctionDescriptor, vertexStaticLinkingDescriptor,
                          MTL_SAMPLER_EMULATION_VERTEX);
         KK_SAMPLER_STAGE(MTL4RenderPipelineDescriptor, fragmentFunctionDescriptor, fragmentStaticLinkingDescriptor,
                          MTL_SAMPLER_EMULATION_FRAGMENT);
      } else if ([copy isKindOfClass:[MTL4MeshRenderPipelineDescriptor class]]) {
         KK_SAMPLER_STAGE(MTL4MeshRenderPipelineDescriptor, objectFunctionDescriptor, objectStaticLinkingDescriptor,
                          MTL_SAMPLER_EMULATION_OBJECT);
         KK_SAMPLER_STAGE(MTL4MeshRenderPipelineDescriptor, meshFunctionDescriptor, meshStaticLinkingDescriptor,
                          MTL_SAMPLER_EMULATION_MESH);
         KK_SAMPLER_STAGE(MTL4MeshRenderPipelineDescriptor, fragmentFunctionDescriptor, fragmentStaticLinkingDescriptor,
                          MTL_SAMPLER_EMULATION_FRAGMENT);
      } else {
         [copy release];
         return NULL;
      }
#undef KK_SAMPLER_STAGE
      return (void *)copy;
   }
}

mtl_render_pipeline_descriptor *
mtl_copy_render_pipeline_descriptor_with_fragment(mtl_render_pipeline_descriptor *descriptor,
                                                  mtl_function_descriptor *fragment)
{
   @autoreleasepool {
      id copy = [(id)descriptor copy];
      if (!copy) return NULL;
      kk_carry_constant_record(copy, (id)descriptor);
      [(MTL4RenderPipelineDescriptor *)copy setFragmentFunctionDescriptor:
         (MTL4FunctionDescriptor *)fragment];
      kk_record_stage_constants(copy, @"fragmentFunctionDescriptor", (id)fragment, false);
      return copy;
   }
}

/* The program a failed pipeline names in its log line: the descriptor's label,
 * else the library function of its first stage, through any specialization. */
static const char *
kk_pipeline_program_label(MTL4PipelineDescriptor *desc)
{
   if (desc.label.length) return desc.label.UTF8String;
   id function = nil;
   for (NSString *key in @[@"computeFunctionDescriptor", @"vertexFunctionDescriptor",
                           @"meshFunctionDescriptor", @"objectFunctionDescriptor",
                           @"fragmentFunctionDescriptor"]) {
      if ([desc respondsToSelector:NSSelectorFromString(key)] &&
          (function = [desc valueForKey:key]))
         break;
   }
   for (unsigned depth = 0; function && depth < 4; ++depth) {
      if ([function isKindOfClass:[MTL4LibraryFunctionDescriptor class]])
         return ((MTL4LibraryFunctionDescriptor *)function).name.UTF8String ?: "(unnamed)";
      if (![function isKindOfClass:[MTL4SpecializedFunctionDescriptor class]]) break;
      function = ((MTL4SpecializedFunctionDescriptor *)function).functionDescriptor;
   }
   return function ? NSStringFromClass([function class]).UTF8String : "(no function)";
}

/* Metal reports an interrupted or timed-out compiler service (for example
 * XPC_ERROR_CONNECTION_INTERRUPTED when the service exits under memory or CPU
 * pressure) as an ordinary failed pipeline. That failure says nothing about
 * the program, so its compile is repeated once. */
static bool
kk_pipeline_error_transient(NSError *error)
{
   NSString *text = error.localizedDescription;
   if (!text) return false;
   for (NSString *mark in @[@"XPC_ERROR", @"interrupted", @"timed out", @"timeout"])
      if ([text rangeOfString:mark options:NSCaseInsensitiveSearch].location != NSNotFound)
         return true;
   return false;
}

/* Compute pipeline */
mtl_compute_pipeline_state *
mtl_new_compute_pipeline_state(mtl_compiler *compiler,
                               mtl_function_descriptor *function,
                               uint64_t max_total_threads_per_threadgroup)
{
   @autoreleasepool {
      KK_FOREGROUND_SCOPE(compiler);

      bool valid = true;
      MTL4FunctionDescriptor *prepared =
         kk_prepare_function_constants((MTL4FunctionDescriptor *)function, &valid);
      if (!valid) return NULL;
      MTL4ComputePipelineDescriptor *desc = [[MTL4ComputePipelineDescriptor new] autorelease];
      desc.computeFunctionDescriptor = prepared;
      kk_record_stage_constants(desc, @"computeFunctionDescriptor", prepared, false);
      desc.maxTotalThreadsPerThreadgroup = max_total_threads_per_threadgroup;

      if (kk_current_operation.cancelled) return NULL;
      NSError *error = nil;
      id<MTLComputePipelineState> pipeline = kk_archive_compute(compiler, desc);
      if (!pipeline) {
         pipeline = MTL_COMPILER_PIPELINE_CALL_DESCRIPTOR(compiler, desc, native,
            [native newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
         if (!pipeline && !kk_current_operation.cancelled && kk_pipeline_error_transient(error)) {
            mesa_loge("MTLComputePipelineState %s: %s; compiling it again",
                      kk_pipeline_program_label(desc), error.localizedDescription.UTF8String);
            error = nil;
            pipeline = MTL_COMPILER_PIPELINE_CALL_DESCRIPTOR(compiler, desc, native,
               [native newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
         }
         mtl_compiler_cache_recapture(compiler, KK_RECAPTURE_BATCH);
      }

      if (kk_current_operation.cancelled) {
         [pipeline release];
         return NULL;
      }
      if (pipeline == nil) {
         mesa_loge("Failed to create MTLComputePipelineState %s: %s",
                   kk_pipeline_program_label(desc), error.localizedDescription.UTF8String);
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
      kk_record_stage_constants(desc, @"vertexFunctionDescriptor", (id)shader, false);
   }
}

void
mtl_render_pipeline_descriptor_set_fragment_shader(mtl_render_pipeline_descriptor *descriptor,
                                                   mtl_function_descriptor *shader)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)shader;
      kk_record_stage_constants(desc, @"fragmentFunctionDescriptor", (id)shader, false);
   }
}

void
mtl_render_pipeline_descriptor_set_private_functions(
   mtl_render_pipeline_descriptor *descriptor, bool vertex, uint32_t count,
   mtl_function_descriptor *const *functions)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (id)descriptor;
      NSMutableArray *dependencies = [NSMutableArray arrayWithCapacity:count];
      for (uint32_t i = 0; i < count; ++i)
         [dependencies addObject:(id)functions[i]];
      MTL4StaticLinkingDescriptor *link = vertex ?
         desc.vertexStaticLinkingDescriptor : desc.fragmentStaticLinkingDescriptor;
      link.privateFunctionDescriptors = dependencies;
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
      MTL4PipelineDescriptor *desc =
         kk_prepare_render_function_constants((MTL4PipelineDescriptor *)descriptor);
      if (!desc) return NULL;

      NSError *error = nil;
      id<MTLRenderPipelineState> pipeline = kk_archive_render(compiler, desc);
      if (!pipeline) {
         pipeline = MTL_COMPILER_PIPELINE_CALL_DESCRIPTOR(compiler, desc, native,
            [native newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
         if (!pipeline && kk_pipeline_error_transient(error)) {
            mesa_loge("MTLRenderPipelineState %s: %s; compiling it again",
                      kk_pipeline_program_label(desc), error.localizedDescription.UTF8String);
            error = nil;
            pipeline = MTL_COMPILER_PIPELINE_CALL_DESCRIPTOR(compiler, desc, native,
               [native newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&error]);
         }
         mtl_compiler_cache_recapture(compiler, KK_RECAPTURE_BATCH);
      }

      if (pipeline == nil) {
         mesa_loge("Failed to create MTLRenderPipelineState %s: %s",
                   kk_pipeline_program_label(desc), error.localizedDescription.UTF8String);
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
      kk_carry_constant_record(copy, (id)descriptor);
      copy.rasterSampleCount = samples;
      /* Mesh pipeline descriptors have no input primitive topology. */
      if (topology != MTL_PRIMITIVE_TOPOLOGY_CLASS_UNSPECIFIED &&
          [copy isKindOfClass:[MTL4RenderPipelineDescriptor class]])
         copy.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)topology;
      if (framebuffer_reads) {
         MTL4SpecializedFunctionDescriptor *fragment =
            kk_new_function_specialization(copy.fragmentFunctionDescriptor);
         if (!fragment) { [copy release]; return NULL; }
         for (uint32_t i = 0; i < count; ++i) {
            bool active = formats[i] != MTL_PIXEL_FORMAT_INVALID;
            kk_set_function_constant(fragment, &active, MTLDataTypeBool, sizeof(active), i);
         }
         copy.fragmentFunctionDescriptor = fragment;
         kk_record_stage_constants(copy, @"fragmentFunctionDescriptor", fragment, true);
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
