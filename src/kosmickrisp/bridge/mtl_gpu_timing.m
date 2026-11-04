/* SPDX-License-Identifier: MIT */
#include "mtl_gpu_timing.h"
#include "util/mesa-blake3.h"
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <string.h>

/* Set only from the once-read kk_perf configuration, never from bridge env. */
bool mtl_gpu_timing_enabled;
static char command_key, library_key, pipeline_key, feedback_key;

@interface KKGPUConfig : NSObject {
@public
   id<MTLDevice> device;
   void *data;
   mtl_gpu_encoder_callback callback;
   uint64_t frequency;
}
@end
@implementation KKGPUConfig

@end

@interface KKGPUCommand : NSObject {
@public
   KKGPUConfig *config;
   NSMutableArray *records;
   uint32_t meta_depth;
   id<MTL4CommandBuffer> command; /* Borrowed; never a retain cycle. */
}
@end
@implementation KKGPUCommand
- (void)dealloc { [config release]; [records release]; [super dealloc]; }
@end

@interface KKGPURecord : NSObject {
@public
   KKGPUConfig *config;
   id<MTL4CommandBuffer> command;
   id<MTL4CounterHeap> heap;
   NSMutableOrderedSet *hashes;
   struct mtl_gpu_encoder_result result;
   bool ended, overlap, extent_mixed;
   unsigned in_flight;
}
@end
@implementation KKGPURecord
- (void)dealloc { [config release]; [heap release]; [hashes release]; [super dealloc]; }
@end

void *
mtl_gpu_timing_configure(mtl_device *device, void *data, mtl_gpu_encoder_callback callback)
{
   @autoreleasepool {
      KKGPUConfig *config = [KKGPUConfig new];
      if (!config) return NULL;
      config->device = (id)device;
      config->data = data;
      config->callback = callback;
      config->frequency = [(id<MTLDevice>)device queryTimestampFrequency];
      __atomic_store_n(&mtl_gpu_timing_enabled, true, __ATOMIC_RELEASE);
      return config;
   }
}

void
mtl_gpu_timing_attach_command(void *context, mtl_command_buffer *command)
{
   KKGPUConfig *config = context;
   if (!config) return;
   KKGPUCommand *trace = [KKGPUCommand new];
   if (!trace) return;
   trace->config = [config retain];
   trace->command = (id)command;
   trace->records = [NSMutableArray new];
   objc_setAssociatedObject((id)command, &command_key, trace, OBJC_ASSOCIATION_RETAIN);
   [trace release];
}

void
mtl_gpu_timing_reset_command(mtl_command_buffer *command)
{
   KKGPUCommand *trace = objc_getAssociatedObject((id)command, &command_key);
   if (trace) {
      /* A submit snapshot retains previous records independently of reset. */
      NSMutableArray *old = trace->records;
      trace->records = [NSMutableArray new];
      trace->meta_depth = 0;
      [old release];
   }
}

static KKGPURecord *
new_record(mtl_command_buffer *command, const char *kind)
{
   KKGPUCommand *trace = objc_getAssociatedObject((id)command, &command_key);
   if (!trace) return nil;
   if (trace->records.count >= 4096) {
      KKGPURecord *overflow = trace->records.lastObject;
      if (trace->records.count == 4096) {
         overflow = [[KKGPURecord new] autorelease];
         overflow->config = [trace->config retain];
         strcpy(overflow->result.kind, "tracking_overflow");
         [trace->records addObject:overflow];
      }
      overflow->result.draws++;
      return nil;
   }
   KKGPURecord *record = [[KKGPURecord new] autorelease];
   if (!record) return nil;
   record->config = [trace->config retain];
   record->command = (id)command;
   record->hashes = [NSMutableOrderedSet new];
   strncpy(record->result.kind, trace->meta_depth ? "meta" : kind, sizeof(record->result.kind)-1);
   MTL4CounterHeapDescriptor *desc = [[MTL4CounterHeapDescriptor new] autorelease];
   desc.type = MTL4CounterHeapTypeTimestamp;
   desc.count = 2;
   record->heap = [trace->config->device newCounterHeapWithDescriptor:desc error:NULL];
   [trace->records addObject:record];
   if (record->heap) [record->command writeTimestampIntoHeap:record->heap atIndex:0];
   return record;
}

void *
mtl_gpu_timing_encoder_begin(mtl_command_buffer *command, bool render, void *descriptor)
{
   KKGPURecord *r = new_record(command, render ? "render" : "compute");
   if (r && render) {
      MTL4RenderPassDescriptor *d = (id)descriptor;
      r->result.width = d.renderTargetWidth;
      r->result.height = d.renderTargetHeight;
      r->result.depth = 1;
      if (!r->result.width || !r->result.height) {
         MTLRenderPassAttachmentDescriptor *a = d.colorAttachments[0].texture ? d.colorAttachments[0] : d.depthAttachment;
         if (a.texture) {
            r->result.width = MAX((NSUInteger)1, a.texture.width >> a.level);
            r->result.height = MAX((NSUInteger)1, a.texture.height >> a.level);
         }
      }
      r->result.extent_known = r->result.width && r->result.height;
   }
   return r;
}

static void
end_record(KKGPURecord *record)
{
   if (!record || record->ended) return;
   if (record->heap) [record->command writeTimestampIntoHeap:record->heap atIndex:1];
   record->ended = true;
}

/* Only enabled encoders use a forwarding receiver. Ordinary draws/binds keep
 * their original native receiver, with no new per-draw/bind branch or lookup.
 * The proxy is never passed into a native commit or used as a native argument. */
@interface KKGPUEncoder : NSProxy {
   id native;
   KKGPURecord *record;
   NSDictionary *bound_pipeline;
}
- (id)initWithNative:(id)encoder record:(KKGPURecord *)r;
@end
@implementation KKGPUEncoder
- (id)initWithNative:(id)encoder record:(KKGPURecord *)r
{ native = encoder; record = [r retain]; return self; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{ return [native methodSignatureForSelector:selector]; }
- (void)forwardInvocation:(NSInvocation *)invocation
{
   const char *selector = sel_getName(invocation.selector);
   if (!strcmp(selector, "setRenderPipelineState:") || !strcmp(selector, "setComputePipelineState:")) {
      id pipeline = nil;
      [invocation getArgument:&pipeline atIndex:2];
      NSDictionary *metadata = objc_getAssociatedObject(pipeline, &pipeline_key);
      [metadata retain]; [bound_pipeline release]; bound_pipeline = metadata;
   }
   if (!strncmp(selector, "draw", 4) || !strncmp(selector, "dispatch", 8)) {
      if (!bound_pipeline) record->result.pipeline_unknown = true;
      for (NSString *hash in bound_pipeline[@"hashes"]) {
         if ([record->hashes containsObject:hash]) continue;
         if (record->hashes.count < MTL_GPU_TIMING_HASHES) [record->hashes addObject:hash];
         else record->result.hashes_truncated = true;
      }
      NSString *kind = bound_pipeline[@"kind"];
      if (kind && ![kind isEqual:@"application"])
         strncpy(record->result.kind, kind.UTF8String, sizeof(record->result.kind)-1);
   }
   if (!strncmp(selector, "draw", 4)) {
      record->result.draws++;
      if (!strncmp(selector, "drawMesh", 8) && !strcmp(record->result.kind, "render"))
         strncpy(record->result.kind, "mesh", sizeof(record->result.kind));
   } else if (!strncmp(selector, "dispatch", 8)) {
      record->result.dispatches++;
      if (!strncmp(selector, "dispatchThreads:", 16) || !strncmp(selector, "dispatchThreadgroups:", 21)) {
         MTLSize size;
         [invocation getArgument:&size atIndex:2];
         if (!strncmp(selector, "dispatchThreadgroups:", 21)) {
            MTLSize threads; [invocation getArgument:&threads atIndex:3];
            size.width *= threads.width; size.height *= threads.height; size.depth *= threads.depth;
         }
         if (record->result.dispatches > 1 &&
             (record->result.width != size.width || record->result.height != size.height || record->result.depth != size.depth))
            record->extent_mixed = true;
         record->result.width = size.width; record->result.height = size.height; record->result.depth = size.depth;
         record->result.extent_known = !record->extent_mixed;
      } else { record->extent_mixed = true; record->result.extent_known = false; }
   } else if (!strncmp(selector, "copy", 4) || !strncmp(selector, "fill", 4)) {
      record->result.blits++;
      if (!record->result.dispatches && !record->result.draws)
         strncpy(record->result.kind, "meta_blit", sizeof(record->result.kind));
   }
   [invocation invokeWithTarget:native];
   if (invocation.selector == @selector(endEncoding)) end_record(record);
}
- (void)dealloc { [native release]; [record release]; [bound_pipeline release]; [super dealloc]; }
@end

void *
mtl_gpu_timing_encoder_wrap(void *record, void *native)
{
   if (!record || !native) return native;
   return [[KKGPUEncoder alloc] initWithNative:(id)native record:record];
}

void *mtl_gpu_timing_external_begin(mtl_command_buffer *command, const char *kind, uint32_t width, uint32_t height)
{
   KKGPURecord *r = new_record(command, kind);
   if (r) { r->result.width = width; r->result.height = height; r->result.depth = 1; r->result.extent_known = width && height; }
   return r;
}
void mtl_gpu_timing_external_end(void *record) { end_record((id)record); }

void *
mtl_gpu_timing_snapshot(mtl_command_buffer **commands, uint32_t count)
{
   NSMutableArray *records = [NSMutableArray new];
   for (uint32_t i = 0; i < count; ++i) {
      KKGPUCommand *trace = objc_getAssociatedObject((id)commands[i], &command_key);
      if (trace) for (KKGPURecord *record in trace->records) {
         @synchronized(record) { if (record->in_flight++) record->overlap = true; }
         [records addObject:record];
      }
   }
   return records;
}

void
mtl_gpu_timing_resolve(void *snapshot, bool success)
{
   for (KKGPURecord *r in (NSArray *)snapshot) {
      @synchronized(r) {
      struct mtl_gpu_encoder_result result = r->result;
      NSData *data = nil;
      @try {
         if (success && !r->overlap && r->ended && r->heap)
            data = [r->heap resolveCounterRange:NSMakeRange(0,2)];
      } @catch (NSException *exception) { (void)exception; }
      if (r->in_flight && !--r->in_flight) r->overlap = false;
      if (data.length == 2 * sizeof(MTL4TimestampHeapEntry) && r->config->frequency) {
         const MTL4TimestampHeapEntry *timestamps = data.bytes;
         result.valid = timestamps[0].timestamp && timestamps[1].timestamp >= timestamps[0].timestamp;
         if (result.valid) result.ms = (double)(timestamps[1].timestamp - timestamps[0].timestamp) * 1000.0 / r->config->frequency;
      }
      for (NSString *hash in [r->hashes.array sortedArrayUsingSelector:@selector(compare:)]) {
         if (result.hash_count == MTL_GPU_TIMING_HASHES) { result.hashes_truncated = true; break; }
         strncpy(result.hashes[result.hash_count++], hash.UTF8String, 64);
      }
      r->config->callback(r->config->data, &result);
      }
   }
}

void
mtl_gpu_timing_library(void *library, const char *source)
{
   if (!library || !source || objc_getAssociatedObject((id)library, &library_key)) return;
   blake3_hash hash; _mesa_blake3_compute(source, strlen(source), hash);
   char hex[65]; _mesa_blake3_format(hex, hash);
   objc_setAssociatedObject((id)library, &library_key, @(hex), OBJC_ASSOCIATION_RETAIN);
}

static void
function_hashes(id function, NSMutableOrderedSet *hashes, bool *meta)
{
   while ([function isKindOfClass:[MTL4SpecializedFunctionDescriptor class]])
      function = [(MTL4SpecializedFunctionDescriptor *)function functionDescriptor];
   if (![function isKindOfClass:[MTL4LibraryFunctionDescriptor class]]) return;
   MTL4LibraryFunctionDescriptor *named = function;
   NSString *hash = objc_getAssociatedObject(named.library, &library_key);
   if (hash) [hashes addObject:hash];
   if ([named.name hasPrefix:@"libkk_"]) *meta = true;
}

void
mtl_gpu_timing_pipeline(void *pipeline, void *descriptor, bool render)
{
   if (!pipeline || !descriptor) return;
   NSMutableOrderedSet *hashes = [NSMutableOrderedSet orderedSet];
   bool meta = false;
   if (!render) function_hashes([(MTL4ComputePipelineDescriptor *)descriptor computeFunctionDescriptor], hashes, &meta);
   else if ([(id)descriptor isKindOfClass:[MTL4MeshRenderPipelineDescriptor class]]) {
      MTL4MeshRenderPipelineDescriptor *d = descriptor;
      function_hashes(d.objectFunctionDescriptor, hashes, &meta);
      function_hashes(d.meshFunctionDescriptor, hashes, &meta);
      function_hashes(d.fragmentFunctionDescriptor, hashes, &meta);
   } else {
      MTL4RenderPipelineDescriptor *d = descriptor;
      function_hashes(d.vertexFunctionDescriptor, hashes, &meta);
      function_hashes(d.fragmentFunctionDescriptor, hashes, &meta);
   }
   objc_setAssociatedObject((id)pipeline, &pipeline_key,
      @{@"hashes":hashes.array, @"kind":meta ? @"meta" : @"application"}, OBJC_ASSOCIATION_RETAIN);
}

void mtl_gpu_timing_pipeline_kind(void *pipeline, const char *kind)
{
   if (!pipeline) return;
   NSDictionary *old = objc_getAssociatedObject((id)pipeline, &pipeline_key);
   objc_setAssociatedObject((id)pipeline, &pipeline_key,
      @{@"hashes": old[@"hashes"] ?: @[], @"kind": @(kind)}, OBJC_ASSOCIATION_RETAIN);
}
void mtl_gpu_timing_meta(mtl_command_buffer *command, bool begin)
{
   KKGPUCommand *trace = objc_getAssociatedObject((id)command, &command_key);
   if (!trace) return;
   if (begin) {
      trace->meta_depth++;
      for (KKGPURecord *record in trace->records)
         if (!record->ended && strcmp(record->result.kind,"tracking_overflow")) strcpy(record->result.kind, "mixed_meta");
   } else if (trace->meta_depth) trace->meta_depth--;
}
void *mtl_gpu_timing_feedback_prepare(void *options)
{
   NSMutableArray *context = [NSMutableArray new];
   objc_setAssociatedObject((id)options, &feedback_key, context, OBJC_ASSOCIATION_RETAIN);
   return context;
}
void mtl_gpu_timing_feedback_capture(void *options, mtl_command_buffer **commands, uint32_t count)
{
   NSMutableArray *context = objc_getAssociatedObject((id)options, &feedback_key);
   if (!context) return;
   NSArray *snapshot = mtl_gpu_timing_snapshot(commands,count);
   [context addObjectsFromArray:snapshot];
   [snapshot release];
}
