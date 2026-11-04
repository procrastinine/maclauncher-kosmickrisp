/* SPDX-License-Identifier: MIT */
/* Private implementation; included only by mtl_compiler.m. */
#include <Metal/MTL4Archive.h>
#include <Metal/MTL4PipelineDataSetSerializer.h>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static char kk_archive_association, kk_library_hint_association;

/* This is only a search hint. Metal still verifies every descriptor, including
 * constants and state not represented here; a collision takes the normal walk. */
static void
kk_remember_library_hint(id library, MTL4LibraryDescriptor *descriptor)
{
   if (!library || !descriptor.source) return;
   NSData *text = [descriptor.source dataUsingEncoding:NSUTF8StringEncoding];
   blake3_hash digest;
   _mesa_blake3_compute(text.bytes, text.length, digest);
   NSArray *fields = @[[NSData dataWithBytes:digest length:sizeof(digest)],
      @(descriptor.options.languageVersion), @(descriptor.options.mathMode),
      @(descriptor.options.mathFloatingPointFunctions)];
   NSData *key = [NSPropertyListSerialization dataWithPropertyList:fields
      format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
   objc_setAssociatedObject(library, &kk_library_hint_association, key,
                            OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static id
kk_function_hint(MTL4FunctionDescriptor *function)
{
   if (!function) return @[];
   if ([function isKindOfClass:[MTL4SpecializedFunctionDescriptor class]])
      function = ((MTL4SpecializedFunctionDescriptor *)function).functionDescriptor;
   if (![function isKindOfClass:[MTL4LibraryFunctionDescriptor class]]) return nil;
   MTL4LibraryFunctionDescriptor *source = (id)function;
   NSData *library = objc_getAssociatedObject(source.library, &kk_library_hint_association);
   return library && source.name ? @[library, source.name] : nil;
}

static NSData *
kk_pipeline_hint(MTL4PipelineDescriptor *descriptor)
{
   NSMutableArray *fields = [NSMutableArray arrayWithObject:@1];
   if ([descriptor isKindOfClass:[MTL4ComputePipelineDescriptor class]]) {
      MTL4ComputePipelineDescriptor *compute = (id)descriptor;
      id function = kk_function_hint(compute.computeFunctionDescriptor);
      if (!function) return nil;
      [fields addObjectsFromArray:@[@"compute", function,
         @(compute.maxTotalThreadsPerThreadgroup)]];
   } else if ([descriptor isKindOfClass:[MTL4RenderPipelineDescriptor class]]) {
      MTL4RenderPipelineDescriptor *render = (id)descriptor;
      id vertex = kk_function_hint(render.vertexFunctionDescriptor);
      id fragment = kk_function_hint(render.fragmentFunctionDescriptor);
      if (!vertex || !fragment) return nil;
      [fields addObjectsFromArray:@[@"render", vertex, fragment,
         @(render.rasterSampleCount), @(render.inputPrimitiveTopology),
         @(render.rasterizationEnabled), @(render.maxVertexAmplificationCount)]];
      /* MTL4 has no depth/stencil-format fields on this descriptor. This
       * advisory key never replaces Metal's complete descriptor verification. */
      for (unsigned i = 0; i < 8; ++i)
         [fields addObject:@[@(render.colorAttachments[i].pixelFormat),
                             @(render.colorAttachments[i].writeMask)]];
   } else {
      /* Unclassified native paths keep the existing complete archive walk. */
      return nil;
   }
   NSData *bytes = [NSPropertyListSerialization dataWithPropertyList:fields
      format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
   if (!bytes) return nil;
   blake3_hash digest;
   _mesa_blake3_compute(bytes.bytes, bytes.length, digest);
   return [NSData dataWithBytes:digest length:sizeof(digest)];
}

static uint64_t
kk_archive_now(void)
{
   struct timespec value;
   if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
   return (uint64_t)value.tv_sec * 1000000000ull + value.tv_nsec;
}

static NSString *
kk_archive_temporary_path(void)
{
   NSString *pattern = [NSTemporaryDirectory() stringByAppendingPathComponent:@"kk-metal-XXXXXX"];
   const char *filesystem_name = pattern.fileSystemRepresentation;
   if (!filesystem_name) return nil;
   char *name = strdup(filesystem_name);
   if (!name) return nil;
   int fd = mkstemp(name);
   NSString *path = fd >= 0 ? [NSString stringWithUTF8String:name] : nil;
   if (fd >= 0) close(fd);
   free(name);
   return path;
}

/* Readers own the archive and backing file across concurrent LRU eviction. */
@interface KKLoadedArchive : NSObject {
@public
   id<MTL4Archive> archive;
   NSString *path;
}
@end
@implementation KKLoadedArchive
- (void)dealloc
{
   [archive release];
   if (path) [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   [path release];
   [super dealloc];
}
@end

@interface KKArchivePiece : NSObject {
@public
   NSData *identity;
   KKLoadedArchive *resident;
   bool unavailable;
}
- (id)initWithIdentity:(NSData *)key;
@end
@implementation KKArchivePiece
- (id)initWithIdentity:(NSData *)key
{
   self = [super init];
   if (self) identity = [key copy];
   return self;
}
- (void)dealloc
{
   [resident release];
   [identity release];
   [super dealloc];
}
@end

/* Bounds cached native objects, not persistent or in-flight coverage. */
#define KK_RESIDENT_ARCHIVE_LIMIT 8

/* Graphics libraries only: bounded source retention and native object count.
 * Specialization and PSO state remain on their original descriptor paths. */
#define KK_GRAPHICS_LIBRARY_LIMIT 128
#define KK_GRAPHICS_SOURCE_LIMIT (8 * 1024 * 1024)

@interface KKGraphicsLibrary : NSObject {
@public
   NSString *source;
   MTLLanguageVersion language;
   MTLMathMode math;
   MTLMathFloatingPointFunctions floating;
   NSCondition *changed;
   id<MTLLibrary> library;
   bool complete;
}
@end
@implementation KKGraphicsLibrary
- (void)dealloc
{
   [source release];
   [changed release];
   [library release];
   [super dealloc];
}
@end

@interface KKPipelineArchive : NSObject {
@public
   id<MTL4PipelineDataSetSerializer> serializer;
   id<MTLDevice> device;
   struct mtl_compiler_cache_ops operations;
   void *cache_data;
   dispatch_once_t loaded;
   pthread_rwlock_t capture_gate;
   dispatch_queue_t checkpoint_queue;
   NSMutableArray *pieces;
   NSMutableDictionary *hints;
   NSMutableDictionary *pending_hints;
   NSMutableDictionary *piece_by_identity;
   NSMutableArray *resident_pieces;
   KKArchivePiece *preferred;
   NSMutableArray *graphics_libraries;
   size_t graphics_source_bytes;
   uint64_t generation, checkpointed, last_compile;
   uint64_t hint_generation, hint_checkpointed;
   bool available, scheduled, finished, gate_ready;
   struct mtl_compiler_cache_stats stats;
}
- (id)initWithDevice:(id<MTLDevice>)dev operations:(const struct mtl_compiler_cache_ops *)ops data:(void *)data;
- (void)load;
- (NSArray *)candidates:(NSData *)hint;
- (KKLoadedArchive *)archiveForPiece:(KKArchivePiece *)piece;
- (void)recordHit:(KKArchivePiece *)piece hint:(NSData *)hint;
- (void)recordCompile:(NSData *)hint;
- (BOOL)publishPiece:(NSData *)identity captured:(NSDictionary *)captured;
- (void)scheduleLocked;
- (void)checkpoint:(BOOL)final;
- (void)finish;
@end

@implementation KKPipelineArchive
- (id)initWithDevice:(id<MTLDevice>)dev operations:(const struct mtl_compiler_cache_ops *)ops data:(void *)data
{
   self = [super init];
   if (!self) return nil;
   if (pthread_rwlock_init(&capture_gate, NULL)) { [self release]; return nil; }
   gate_ready = true;
   device = [dev retain];
   operations = *ops;
   cache_data = data;
   pieces = [[NSMutableArray alloc] init];
   hints = [NSMutableDictionary new];
   pending_hints = [NSMutableDictionary new];
   piece_by_identity = [NSMutableDictionary new];
   resident_pieces = [NSMutableArray new];
   MTL4PipelineDataSetSerializerDescriptor *desc = [[MTL4PipelineDataSetSerializerDescriptor new] autorelease];
   desc.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
   serializer = [dev newPipelineDataSetSerializerWithDescriptor:desc];
   return self;
}

- (void)load
{
   dispatch_once(&loaded, ^{
      available = operations.load_piece && operations.store_piece &&
         (!operations.available || operations.available(cache_data));
      if (!available) return;
      size_t length = 0;
      void *bytes = operations.load(cache_data, &length);
      if (!bytes) return;
      NSData *data = [NSData dataWithBytesNoCopy:bytes length:length freeWhenDone:YES];
      id index = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL];
      NSArray *rows = nil;
      if ([index isKindOfClass:[NSDictionary class]]) {
         if (![index[@"version"] isEqual:@1]) return;
         rows = index[@"hints"];
         index = index[@"pieces"];
      }
      if (![index isKindOfClass:[NSArray class]]) return;
      /* The index contains only content identifiers, never object graphs. */
      for (id key in index) {
         if (![key isKindOfClass:[NSData class]] || [key length] != 32) {
            [pieces removeAllObjects];
            [piece_by_identity removeAllObjects];
            return;
         }
         if (piece_by_identity[key]) continue;
         KKArchivePiece *piece = [[KKArchivePiece alloc] initWithIdentity:key];
         if (!piece) { [pieces removeAllObjects]; [piece_by_identity removeAllObjects]; return; }
         [pieces addObject:piece];
         piece_by_identity[key] = piece;
         [piece release];
      }
      if ([rows isKindOfClass:[NSArray class]]) for (id row in rows) {
         if (![row isKindOfClass:[NSArray class]] || [row count] != 2) continue;
         id hint = row[0], key = row[1];
         if ([hint isKindOfClass:[NSData class]] && [hint length] == 32 &&
             [key isKindOfClass:[NSData class]] && [key length] == 32 &&
             piece_by_identity[key])
            hints[hint] = key;
      }
   });
}

- (NSArray *)candidates:(NSData *)hint
{
   [self load];
   @synchronized(self) {
      NSMutableArray *result = [NSMutableArray arrayWithCapacity:pieces.count];
      NSData *identity = hint ? hints[hint] : nil;
      KKArchivePiece *hinted = identity ? piece_by_identity[identity] : nil;
      if (hinted) [result addObject:hinted];
      if (preferred && preferred != hinted) [result addObject:preferred];
      for (KKArchivePiece *piece in [pieces reverseObjectEnumerator])
         if (piece != preferred && piece != hinted) [result addObject:piece];
      return [[result copy] autorelease];
   }
}

- (KKLoadedArchive *)archiveForPiece:(KKArchivePiece *)piece
{
   KKLoadedArchive *result = nil;
   /* No global publication lock during disk IO or native archive opening. */
   @synchronized(piece) {
      if (!piece->resident && !piece->unavailable) {
         size_t length = 0;
         void *bytes = operations.load_piece(cache_data, piece->identity.bytes, &length);
         NSData *data = bytes
            ? [NSData dataWithBytesNoCopy:bytes length:length freeWhenDone:YES] : nil;
         KKLoadedArchive *opened = [KKLoadedArchive new];
         if (!opened) return nil;
         NSString *path = data ? kk_archive_temporary_path() : nil;
         NSError *error = nil;
         if (path && [data writeToFile:path options:NSDataWritingAtomic error:&error]) {
            opened->archive = [device newArchiveWithURL:[NSURL fileURLWithPath:path] error:&error];
            opened->path = [path copy];
         }
         if (opened->archive) piece->resident = opened;
         else {
            [opened release];
            if (path) [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
            piece->unavailable = true; /* Disk eviction/corruption is a miss. */
         }
      }
      result = [piece->resident retain];
   }
   KKArchivePiece *retired = nil;
   if (result) @synchronized(self) {
      stats.archive_loaded = true;
      [resident_pieces removeObjectIdenticalTo:piece];
      [resident_pieces addObject:piece];
      if (resident_pieces.count > KK_RESIDENT_ARCHIVE_LIMIT) {
         retired = [resident_pieces[0] retain];
         [resident_pieces removeObjectAtIndex:0];
      }
   }
   if (retired) {
      @synchronized(retired) {
         [retired->resident release];
         retired->resident = nil;
      }
      [retired release];
   }
   return [result autorelease];
}

- (void)recordHit:(KKArchivePiece *)piece hint:(NSData *)hint
{
   @synchronized(self) {
      stats.hits++;
      if (hint && ![hints[hint] isEqual:piece->identity]) {
         hints[hint] = piece->identity;
         hint_generation++;
         [self scheduleLocked];
      }
      if (preferred != piece) {
         [preferred release];
         preferred = [piece retain];
      }
   }
}

/* Caller holds capture_gate for reading until after this publication. */
- (void)recordCompile:(NSData *)hint
{
   @synchronized(self) {
      stats.native_compiles++;
      if (!serializer || !available || finished) return;
      generation++;
      if (hint) pending_hints[hint] = @YES;
      last_compile = kk_archive_now();
      [self scheduleLocked];
   }
}

- (void)scheduleLocked
{
   if (scheduled || finished ||
       (generation == checkpointed && hint_generation == hint_checkpointed)) return;
   if (!checkpoint_queue) {
      dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(
         DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
      checkpoint_queue = dispatch_queue_create("kosmickrisp.archive", attributes);
   }
   if (!checkpoint_queue) return;
   scheduled = true;
   /* The copied block keeps the cache alive even if Stop/device teardown races
    * its delay; it never captures or dereferences the native compiler. */
   dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500000000), checkpoint_queue, ^{
      @autoreleasepool { [self checkpoint:NO]; }
   });
}

/* The utility queue is the sole index writer. Piece bytes are verified before
 * the two-slot journal can publish both their reference and captured hints. */
- (BOOL)publishPiece:(NSData *)identity captured:(NSDictionary *)captured
{
   NSMutableArray *index = [NSMutableArray array], *rows = [NSMutableArray array];
   NSMutableDictionary *directory;
   uint64_t hint_target;
   @synchronized(self) {
      for (KKArchivePiece *entry in pieces) [index addObject:entry->identity];
      if (identity && !piece_by_identity[identity]) [index addObject:identity];
      directory = [[hints mutableCopy] autorelease];
      hint_target = hint_generation;
   }
   if (identity) for (NSData *hint in captured) directory[hint] = identity;
   for (NSData *hint in directory) [rows addObject:@[hint, directory[hint]]];
   NSData *manifest = [NSPropertyListSerialization dataWithPropertyList:
      @{@"version": @1, @"pieces": index, @"hints": rows}
      format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
   BOOL stored = manifest.length &&
      operations.store(cache_data, manifest.bytes, manifest.length);
   @synchronized(self) {
      if (stored) {
         if (identity && !piece_by_identity[identity]) {
            KKArchivePiece *piece = [[KKArchivePiece alloc] initWithIdentity:identity];
            if (piece) {
               [pieces addObject:piece];
               piece_by_identity[identity] = piece;
               [piece release];
            }
         }
         if (identity) for (NSData *hint in captured) hints[hint] = identity;
         stats.checkpoints++;
      }
      /* Storage failure is an ordinary cache miss, not a timed retry loop.
       * Updates received during IO remain dirty and get another transaction. */
      hint_checkpointed = hint_target;
   }
   return stored;
}

- (void)checkpoint:(BOOL)final
{
   uint64_t target;
   bool capture;
   @synchronized(self) {
      if (!final) scheduled = false;
      if ((!final && finished) || !serializer || !available ||
          (generation == checkpointed && hint_generation == hint_checkpointed))
         return;
      capture = generation != checkpointed;
      uint64_t now = kk_archive_now();
      if (!final && capture && generation - checkpointed < 16 &&
          now && last_compile && now - last_compile < 500000000) {
         [self scheduleLocked];
         return;
      }
      target = checkpointed;
   }
   NSData *data = nil;
   NSDictionary *captured = nil;
   if (capture) {
      /* Captures share read access; only the short snapshot is exclusive. */
      if (final) pthread_rwlock_wrlock(&capture_gate);
      else if (pthread_rwlock_trywrlock(&capture_gate)) {
         @synchronized(self) { [self scheduleLocked]; }
         return;
      }
      @synchronized(self) {
         target = generation;
         captured = [pending_hints copy];
         [pending_hints removeAllObjects];
      }
      NSString *path = kk_archive_temporary_path();
      NSError *error = nil;
      BOOL serialized = NO;
      uint64_t begin = kk_archive_now();
      @try {
         if (path) {
            [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
            serialized = [serializer serializeAsArchiveAndFlushToURL:
               [NSURL fileURLWithPath:path] error:&error];
         }
      } @catch (NSException *exception) {
         serialized = NO;
      } @finally {
         pthread_rwlock_unlock(&capture_gate);
      }
      @synchronized(self) { stats.snapshot_ns += kk_archive_now() - begin; }
      /* All IO, hashing, piece/index storage and verification follow unlock. */
      data = serialized ? [NSData dataWithContentsOfFile:path] : nil;
      if (path) [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   }
   NSData *identity = nil;
   if (data.length) {
      uint8_t key[32];
      if (operations.store_piece(cache_data, data.bytes, data.length, key))
         identity = [NSData dataWithBytes:key length:sizeof(key)];
   }
   BOOL stored = [self publishPiece:identity captured:identity ? captured : nil];
   if (stored && identity) @synchronized(self) { stats.stored_bytes += data.length; }
   [captured release];
   @synchronized(self) {
      checkpointed = target;
      if (!final) [self scheduleLocked];
   }
}

- (void)finish
{
   dispatch_queue_t queue;
   @synchronized(self) {
      if (finished) return;
      finished = true;
      queue = checkpoint_queue;
   }
   if (queue) dispatch_sync(queue, ^{ @autoreleasepool { [self checkpoint:YES]; } });
   else [self checkpoint:YES];
}

- (void)dealloc
{
   [graphics_libraries release];
   [preferred release];
   [pieces release];
   [hints release];
   [pending_hints release];
   [piece_by_identity release];
   [resident_pieces release];
   [serializer release];
   [device release];
   if (checkpoint_queue) dispatch_release(checkpoint_queue);
   if (gate_ready) pthread_rwlock_destroy(&capture_gate);
   if (operations.destroy) operations.destroy(cache_data);
   [super dealloc];
}
@end

static KKPipelineArchive *
kk_compiler_archive(mtl_compiler *compiler)
{
   return objc_getAssociatedObject((id)compiler, &kk_archive_association);
}

/* Pending entries are created only when a KK worker/caller enters a compile,
 * never while enqueueing. Completed-only probes cannot wait. Thus pending entries cannot depend on queued work
 * behind their waiters. Native calls and waits hold no archive/cache lock. */
static id<MTLLibrary>
kk_graphics_library(mtl_compiler *compiler, MTL4LibraryDescriptor *desc, bool vertex, bool completed_only)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   KKGraphicsLibrary *entry = nil;
   NSMutableArray *retired = nil;
   bool owner = false;
   size_t bytes = desc.source.length * sizeof(unichar);
   if (cache && desc.source && bytes <= KK_GRAPHICS_SOURCE_LIMIT) {
      @synchronized(cache) {
         if (vertex && !completed_only) cache->stats.vertex_library_requests++;
         for (KKGraphicsLibrary *candidate in cache->graphics_libraries) {
            if (completed_only && !__atomic_load_n(&candidate->complete, __ATOMIC_ACQUIRE)) continue;
            if (candidate->language == desc.options.languageVersion &&
                candidate->math == desc.options.mathMode &&
                candidate->floating == desc.options.mathFloatingPointFunctions &&
                [candidate->source isEqualToString:desc.source]) {
               entry = [candidate retain];
               if (vertex) {
                  if (completed_only) cache->stats.vertex_library_requests++;
                  cache->stats.vertex_library_hits++;
               }
               break;
            }
         }
         if (!entry && !completed_only) {
            /* FIFO eviction skips active compiles. If every slot is active,
             * use the ordinary synchronous path without retaining more state. */
            retired = [[NSMutableArray alloc] init];
            for (NSUInteger i = 0; i < cache->graphics_libraries.count &&
                 (cache->graphics_libraries.count >= KK_GRAPHICS_LIBRARY_LIMIT ||
                  cache->graphics_source_bytes + bytes > KK_GRAPHICS_SOURCE_LIMIT);) {
               KKGraphicsLibrary *old = cache->graphics_libraries[i];
               if (!__atomic_load_n(&old->complete, __ATOMIC_ACQUIRE)) { ++i; continue; }
               [retired addObject:old];
               cache->graphics_source_bytes -= old->source.length * sizeof(unichar);
               [cache->graphics_libraries removeObjectAtIndex:i];
            }
            if (cache->graphics_libraries.count < KK_GRAPHICS_LIBRARY_LIMIT &&
                cache->graphics_source_bytes + bytes <= KK_GRAPHICS_SOURCE_LIMIT) {
               if (!cache->graphics_libraries) cache->graphics_libraries = [NSMutableArray new];
               entry = [KKGraphicsLibrary new];
               if (entry) {
                  entry->source = [desc.source copy];
                  entry->language = desc.options.languageVersion;
                  entry->math = desc.options.mathMode;
                  entry->floating = desc.options.mathFloatingPointFunctions;
                  entry->changed = [NSCondition new];
                  if (entry->source && entry->changed && cache->graphics_libraries) {
                     [cache->graphics_libraries addObject:entry];
                     cache->graphics_source_bytes += bytes;
                     owner = true;
                  } else {
                     [entry release];
                     entry = nil;
                  }
               }
            }
         }
      }
   }
   [retired release]; /* Native destruction outside cache publication lock. */
   if (!entry && completed_only) return nil;
   if (entry && !owner) {
      [entry->changed lock];
      while (!__atomic_load_n(&entry->complete, __ATOMIC_ACQUIRE)) [entry->changed wait];
      id<MTLLibrary> result = [entry->library retain];
      [entry->changed unlock];
      [entry release];
      return result;
   }
   id<MTLLibrary> result = nil;
   @try {
      NSError *error = nil;
      result = [(id<MTL4Compiler>)compiler newLibraryWithDescriptor:desc error:&error];
      if (cache) kk_remember_library_hint(result, desc);
      if (!result)
         fprintf(stderr, "Failed to create MTLLibrary: %s\n",
                 error.localizedDescription.UTF8String ?: "Unknown Metal error");
   } @finally {
      if (entry) {
         /* A failure wakes existing waiters but is never memoized. A later
          * request retries transient native errors with the same exact input. */
         if (!result) @synchronized(cache) {
            cache->graphics_source_bytes -= bytes;
            [cache->graphics_libraries removeObjectIdenticalTo:entry];
         }
         [entry->changed lock];
         entry->library = [result retain];
         __atomic_store_n(&entry->complete, true, __ATOMIC_RELEASE);
         [entry->changed broadcast];
         [entry->changed unlock];
         [entry release];
      }
   }
   return result;
}

void *
mtl_compiler_cache_begin(mtl_compiler *compiler)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return NULL;
   [cache load];
   if (!cache->available || !cache->serializer) return NULL;
   if (pthread_rwlock_tryrdlock(&cache->capture_gate)) {
      uint64_t begin = kk_archive_now();
      pthread_rwlock_rdlock(&cache->capture_gate);
      uint64_t duration = kk_archive_now() - begin;
      @synchronized(cache) { cache->stats.capture_wait_ns += duration; }
   }
   return cache;
}

void
mtl_compiler_cache_end(void *token)
{
   if (token) pthread_rwlock_unlock(&((KKPipelineArchive *)token)->capture_gate);
}

void
mtl_compiler_cache_record_pipeline(mtl_compiler *compiler)
{
   [kk_compiler_archive(compiler) recordCompile:nil];
}

/* Called before the capture read token is released, so hints and captured
 * binaries cannot be separated by a concurrent serializer checkpoint. */
void
mtl_compiler_cache_record_descriptor(mtl_compiler *compiler, void *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   [cache recordCompile:cache && cache->available && descriptor ? kk_pipeline_hint((id)descriptor) : nil];
}

static id<MTLComputePipelineState>
kk_archive_compute(mtl_compiler *compiler, MTL4ComputePipelineDescriptor *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return nil;
   @synchronized(cache) { cache->stats.requests++; }
   NSData *hint = kk_pipeline_hint(descriptor);
   for (KKArchivePiece *piece in [cache candidates:hint]) {
      KKLoadedArchive *loaded = [cache archiveForPiece:piece];
      if (!loaded) continue;
      id<MTL4Archive> archive = loaded->archive;
      @synchronized(cache) { cache->stats.lookups++; }
      NSError *error = nil;
      id<MTLComputePipelineState> state = [archive newComputePipelineStateWithDescriptor:descriptor error:&error];
      if (state) { [cache recordHit:piece hint:hint]; return state; }
   }
   return nil;
}

static id<MTLRenderPipelineState>
kk_archive_render(mtl_compiler *compiler, MTL4PipelineDescriptor *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return nil;
   @synchronized(cache) { cache->stats.requests++; }
   NSData *hint = kk_pipeline_hint(descriptor);
   for (KKArchivePiece *piece in [cache candidates:hint]) {
      KKLoadedArchive *loaded = [cache archiveForPiece:piece];
      if (!loaded) continue;
      id<MTL4Archive> archive = loaded->archive;
      @synchronized(cache) { cache->stats.lookups++; }
      NSError *error = nil;
      id<MTLRenderPipelineState> state = [archive newRenderPipelineStateWithDescriptor:descriptor error:&error];
      if (state) { [cache recordHit:piece hint:hint]; return state; }
   }
   return nil;
}

bool
mtl_compiler_get_cache_stats(mtl_compiler *compiler,
                            struct mtl_compiler_cache_stats *stats)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return false;
   @synchronized(cache) {
      *stats = cache->stats;

   }
   return true;
}
