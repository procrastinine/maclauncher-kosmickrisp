/* SPDX-License-Identifier: MIT */
#include "mtl_gpu_timing.h"
#include "util/mesa-blake3.h"
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <os/lock.h>
#include <string.h>

/* Set only from the once-read kk_perf configuration, never from bridge env. */
bool mtl_gpu_timing_enabled;
static char command_key, library_key, pipeline_key, feedback_key, encoder_key;

/* AGX allows a process 32 live timestamp heaps, shared with the application's timestamp query
 * pools; past that the allocation fails, and faults when its error argument is NULL. Metal
 * rejects heaps over 4096 entries (32 KB). A device therefore times encoders through at most two
 * heaps of 4096 entries, allocated on first use, and each encoder takes a begin/end slot pair
 * that returns to the free list when its record is released. With every slot in use, or after
 * Metal refused a heap, an encoder is reported as unavailable; recording never waits. */
#define KK_GPU_TIMING_HEAP_ENTRIES 4096
#define KK_GPU_TIMING_HEAPS 2
#define KK_GPU_TIMING_SLOTS (KK_GPU_TIMING_HEAPS * KK_GPU_TIMING_HEAP_ENTRIES / 2)

@interface KKGPUConfig : NSObject {
@public
   id<MTLDevice> device;
   void *data;
   mtl_gpu_encoder_callback callback;
   uint64_t frequency;
   os_unfair_lock slot_lock;
   id<MTL4CounterHeap> heaps[KK_GPU_TIMING_HEAPS];
   uint32_t heap_count, free_count;
   bool heap_failed;
   uint32_t free_slots[KK_GPU_TIMING_SLOTS];
}
@end
@implementation KKGPUConfig
- (void)dealloc
{
   for (uint32_t i = 0; i < heap_count; ++i) [heaps[i] release];
   [super dealloc];
}
@end

static bool
take_slot(KKGPUConfig *config, uint32_t *slot)
{
   os_unfair_lock_lock(&config->slot_lock);
   if (!config->free_count && !config->heap_failed && config->heap_count < KK_GPU_TIMING_HEAPS) {
      MTL4CounterHeapDescriptor *desc = [MTL4CounterHeapDescriptor new];
      desc.type = MTL4CounterHeapTypeTimestamp;
      desc.count = KK_GPU_TIMING_HEAP_ENTRIES;
      NSError *error = nil;
      id<MTL4CounterHeap> heap = [config->device newCounterHeapWithDescriptor:desc error:&error];
      [desc release];
      if (heap) {
         const uint32_t base = config->heap_count * (KK_GPU_TIMING_HEAP_ENTRIES / 2);
         config->heaps[config->heap_count++] = heap;
         for (uint32_t i = KK_GPU_TIMING_HEAP_ENTRIES / 2; i--;)
            config->free_slots[config->free_count++] = base + i;
      } else {
         config->heap_failed = true;
      }
   }
   const bool taken = config->free_count != 0;
   if (taken) *slot = config->free_slots[--config->free_count];
   os_unfair_lock_unlock(&config->slot_lock);
   return taken;
}

static void
give_slot(KKGPUConfig *config, uint32_t slot)
{
   os_unfair_lock_lock(&config->slot_lock);
   config->free_slots[config->free_count++] = slot;
   os_unfair_lock_unlock(&config->slot_lock);
}

static inline id<MTL4CounterHeap>
slot_heap(KKGPUConfig *config, uint32_t slot)
{
   return config->heaps[slot / (KK_GPU_TIMING_HEAP_ENTRIES / 2)];
}

static inline uint32_t
slot_index(uint32_t slot)
{
   return slot % (KK_GPU_TIMING_HEAP_ENTRIES / 2) * 2;
}

@interface KKGPUCommand : NSObject {
@public
   KKGPUConfig *config;
   NSMutableArray *records;
   uint32_t meta_depth;
}
@end
@implementation KKGPUCommand
- (void)dealloc { [config release]; [records release]; [super dealloc]; }
@end

@interface KKGPURecord : NSObject {
@public
   KKGPUConfig *config;
   /* Borrowed and used only while the buffer records: an encoder record writes its timestamps
    * into its encoder, an external record into the command buffer between encoders. */
   id<MTL4CommandBuffer> command;
   id native; /* The encoder of an encoder record, from bind to its end. */
   uint32_t slot;
   bool has_slot, encoder, render;
   NSDictionary *bound;  /* Metadata of the bound pipeline. */
   NSDictionary *merged; /* The bound metadata whose hashes are in hashes. */
   char bound_kind[32];  /* Its kind when not "application"; empty otherwise. */
   NSMutableOrderedSet *hashes;
   struct mtl_gpu_encoder_result result;
   bool ended, overlap, extent_mixed;
   bool unbound; /* An encoder record ended without its encoder: no timestamps. */
   unsigned in_flight;
}
@end
@implementation KKGPURecord
- (void)dealloc
{
   if (has_slot) give_slot(config, slot);
   [config release]; [bound release]; [merged release]; [hashes release]; [super dealloc];
}
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
      config->slot_lock = OS_UNFAIR_LOCK_INIT;
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
new_record(mtl_command_buffer *command, const char *kind, bool encoder)
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
   strncpy(record->result.kind, trace->meta_depth ? "meta" : kind, sizeof(record->result.kind)-1);
   record->has_slot = take_slot(record->config, &record->slot);
   record->encoder = encoder;
   [trace->records addObject:record];
   if (record->has_slot && !encoder)
      [record->command writeTimestampIntoHeap:slot_heap(record->config, record->slot)
                                      atIndex:slot_index(record->slot)];
   return record;
}

void *
mtl_gpu_timing_encoder_begin(mtl_command_buffer *command, bool render, void *descriptor)
{
   KKGPURecord *r = new_record(command, render ? "render" : "compute", true);
   if (r) r->render = render;
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

/* Encoder timestamps use relaxed granularity, which never splits the encoder; on a render
 * encoder the first follows the vertex stage of nothing and the last the fragment stage of all
 * of its work. Writing them inside the encoder costs a fraction of a command buffer timestamp
 * between encoders. */
static void
write_encoder_timestamp(KKGPURecord *record, bool end)
{
   id<MTL4CounterHeap> heap = slot_heap(record->config, record->slot);
   const uint32_t index = slot_index(record->slot) + end;
   if (record->render)
      [(id<MTL4RenderCommandEncoder>)record->native
         writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed
                            afterStage:end ? MTLRenderStageFragment : MTLRenderStageVertex
                              intoHeap:heap
                               atIndex:index];
   else
      [(id<MTL4ComputeCommandEncoder>)record->native
         writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed
                              intoHeap:heap
                               atIndex:index];
}

static void
end_record(KKGPURecord *record)
{
   if (!record || record->ended) return;
   if (record->encoder) {
      if (record->native && record->has_slot) write_encoder_timestamp(record, true);
      record->unbound = !record->native;
   } else if (record->has_slot) {
      [record->command writeTimestampIntoHeap:slot_heap(record->config, record->slot)
                                      atIndex:slot_index(record->slot) + 1];
   }
   record->native = nil;
   record->ended = true;
}

/* The record of an encoder being recorded: associated with the native encoder from creation to
 * endEncoding (Metal may hand out the same encoder object again afterwards), and remembered for
 * the recording thread's latest encoder. Only the timing entry points look it up; with timing
 * off, the encoder entry points test one flag (mtl_gpu_timing_on). */
static _Thread_local struct {
   void *encoder;
   KKGPURecord *record; /* Retained. */
} current;

static void
remember(void *encoder, KKGPURecord *record)
{
   if (current.record != record) {
      [record retain];
      [current.record release];
      current.record = record;
   }
   current.encoder = record ? encoder : NULL;
}

static KKGPURecord *
encoder_record(void *encoder)
{
   if (!encoder) return nil;
   if (current.encoder == encoder && !current.record->ended) return current.record;
   KKGPURecord *record = objc_getAssociatedObject((id)encoder, &encoder_key);
   if (record && record->ended) record = nil;
   remember(encoder, record);
   return record;
}

void
mtl_gpu_timing_encoder_bind(void *record, void *encoder)
{
   KKGPURecord *r = record;
   if (!r || !encoder) return;
   r->native = (id)encoder;
   if (r->has_slot) write_encoder_timestamp(r, false);
   objc_setAssociatedObject((id)encoder, &encoder_key, (id)record, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   remember(encoder, record);
}

/* Before the encoder's endEncoding: writes its last timestamp. */
void
mtl_gpu_timing_encoder_end(void *encoder)
{
   KKGPURecord *record = encoder_record(encoder);
   if (!record) return;
   end_record(record);
   objc_setAssociatedObject((id)encoder, &encoder_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   remember(NULL, nil);
}

void
mtl_gpu_timing_bind_pipeline(void *encoder, void *pipeline)
{
   KKGPURecord *record = encoder_record(encoder);
   if (!record) return;
   NSDictionary *metadata = pipeline ? objc_getAssociatedObject((id)pipeline, &pipeline_key) : nil;
   [metadata retain]; [record->bound release]; record->bound = metadata;
}

void
mtl_gpu_timing_work(void *encoder, enum mtl_gpu_timing_work work,
                    uint32_t width, uint32_t height, uint32_t depth)
{
   KKGPURecord *record = encoder_record(encoder);
   if (!record) return;
   struct mtl_gpu_encoder_result *result = &record->result;
   if (work != MTL_GPU_TIMING_BLIT) {
      NSDictionary *bound = record->bound;
      if (!bound) result->pipeline_unknown = true;
      else if (bound != record->merged) {
         if (!record->hashes) record->hashes = [NSMutableOrderedSet new];
         for (NSString *hash in bound[@"hashes"]) {
            if ([record->hashes containsObject:hash]) continue;
            if (record->hashes.count < MTL_GPU_TIMING_HASHES) [record->hashes addObject:hash];
            else result->hashes_truncated = true;
         }
         NSString *kind = bound[@"kind"];
         record->bound_kind[0] = 0;
         if (kind && ![kind isEqual:@"application"])
            strncpy(record->bound_kind, kind.UTF8String, sizeof(record->bound_kind)-1);
         [bound retain]; [record->merged release]; record->merged = bound;
      }
      if (bound && record->bound_kind[0])
         memcpy(result->kind, record->bound_kind, sizeof(result->kind));
   }
   switch (work) {
   case MTL_GPU_TIMING_MESH_DRAW:
      if (!strcmp(result->kind, "render"))
         strncpy(result->kind, "mesh", sizeof(result->kind));
      result->draws++;
      break;
   case MTL_GPU_TIMING_DRAW:
      result->draws++;
      break;
   case MTL_GPU_TIMING_DISPATCH:
      result->dispatches++;
      if (result->dispatches > 1 &&
          (result->width != width || result->height != height || result->depth != depth))
         record->extent_mixed = true;
      result->width = width; result->height = height; result->depth = depth;
      result->extent_known = !record->extent_mixed;
      break;
   case MTL_GPU_TIMING_INDIRECT_DISPATCH:
      result->dispatches++;
      record->extent_mixed = true; result->extent_known = false;
      break;
   case MTL_GPU_TIMING_BLIT:
      result->blits++;
      if (!result->dispatches && !result->draws)
         strncpy(result->kind, "meta_blit", sizeof(result->kind));
      break;
   }
}

void *mtl_gpu_timing_external_begin(mtl_command_buffer *command, const char *kind, uint32_t width, uint32_t height)
{
   KKGPURecord *r = new_record(command, kind, false);
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
   /* One CPU resolve per heap per snapshot, over the entries its records use. */
   KKGPUConfig *config = nil;
   NSData *data[KK_GPU_TIMING_HEAPS] = { nil };
   uint32_t used[KK_GPU_TIMING_HEAPS] = { 0 };
   for (KKGPURecord *r in (NSArray *)snapshot) {
      if (!r->has_slot) continue;
      if (!config) config = r->config;
      if (r->config != config) continue; /* One device per submission. */
      const uint32_t heap = r->slot / (KK_GPU_TIMING_HEAP_ENTRIES / 2);
      used[heap] = MAX(used[heap], slot_index(r->slot) + 2);
   }
   for (KKGPURecord *r in (NSArray *)snapshot) {
      @synchronized(r) {
      struct mtl_gpu_encoder_result result = r->result;
      const MTL4TimestampHeapEntry *timestamps = NULL;
      if (success && !r->overlap && r->ended && r->has_slot && r->config == config && !r->unbound) {
         const uint32_t heap = r->slot / (KK_GPU_TIMING_HEAP_ENTRIES / 2);
         @try {
            if (!data[heap])
               data[heap] = [slot_heap(config, r->slot) resolveCounterRange:NSMakeRange(0, used[heap])];
         } @catch (NSException *exception) { (void)exception; }
         if (data[heap].length == used[heap] * sizeof(MTL4TimestampHeapEntry))
            timestamps = (const MTL4TimestampHeapEntry *)data[heap].bytes + slot_index(r->slot);
      }
      if (r->in_flight && !--r->in_flight) r->overlap = false;
      if (timestamps && r->config->frequency) {
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
