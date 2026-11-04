/* SPDX-License-Identifier: MIT */
/* Private implementation; included only by mtl_compiler.m. */
#include <Metal/MTL4Archive.h>
#include <Metal/MTL4PipelineDataSetSerializer.h>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static char kk_archive_association;

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

@interface KKArchivePiece : NSObject {
@public
   NSData *identity;
   dispatch_once_t loaded;
   id<MTL4Archive> archive;
   NSString *path;
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
   [archive release];
   if (path) [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   [path release];
   [identity release];
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
   KKArchivePiece *preferred;
   uint64_t generation, checkpointed, last_compile;
   bool available, scheduled, finished, gate_ready;
   struct mtl_compiler_cache_stats stats;
}
- (id)initWithDevice:(id<MTLDevice>)dev operations:(const struct mtl_compiler_cache_ops *)ops data:(void *)data;
- (void)load;
- (NSArray *)candidates;
- (id<MTL4Archive>)archiveForPiece:(KKArchivePiece *)piece;
- (void)recordHit:(KKArchivePiece *)piece;
- (void)recordCompile;
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
      if (![index isKindOfClass:[NSArray class]] || [index count] > 64) return;
      /* The index contains only content identifiers, never object graphs. */
      for (id key in index) {
         if (![key isKindOfClass:[NSData class]] || [key length] != 32) {
            [pieces removeAllObjects];
            return;
         }
         KKArchivePiece *piece = [[KKArchivePiece alloc] initWithIdentity:key];
         if (!piece) { [pieces removeAllObjects]; return; }
         [pieces addObject:piece];
         [piece release];
      }
   });
}

- (NSArray *)candidates
{
   [self load];
   @synchronized(self) {
      NSMutableArray *result = [NSMutableArray arrayWithCapacity:pieces.count];
      if (preferred) [result addObject:preferred];
      for (KKArchivePiece *piece in [pieces reverseObjectEnumerator])
         if (piece != preferred) [result addObject:piece];
      return [[result copy] autorelease];
   }
}

- (id<MTL4Archive>)archiveForPiece:(KKArchivePiece *)piece
{
   dispatch_once(&piece->loaded, ^{
      size_t length = 0;
      void *bytes = operations.load_piece(cache_data, piece->identity.bytes, &length);
      if (!bytes) return;
      NSData *data = [NSData dataWithBytesNoCopy:bytes length:length freeWhenDone:YES];
      NSString *path = kk_archive_temporary_path();
      NSError *error = nil;
      if (path && [data writeToFile:path options:NSDataWritingAtomic error:&error]) {
         piece->archive = [device newArchiveWithURL:[NSURL fileURLWithPath:path] error:&error];
         if (piece->archive) piece->path = [path copy];
      }
      if (!piece->archive && path)
         [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   });
   if (piece->archive) @synchronized(self) { stats.archive_loaded = true; }
   return piece->archive;
}

- (void)recordHit:(KKArchivePiece *)piece
{
   @synchronized(self) {
      stats.hits++;
      if (preferred != piece) {
         [preferred release];
         preferred = [piece retain];
      }
   }
}

/* Caller holds capture_gate for reading until after this publication. */
- (void)recordCompile
{
   @synchronized(self) {
      stats.native_compiles++;
      if (!serializer || !available || finished) return;
      generation++;
      last_compile = kk_archive_now();
      [self scheduleLocked];
   }
}

- (void)scheduleLocked
{
   if (scheduled || finished || generation == checkpointed) return;
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

- (void)checkpoint:(BOOL)final
{
   uint64_t target;
   @synchronized(self) {
      if (!final) scheduled = false;
      if ((!final && finished) || !serializer || !available || generation == checkpointed)
         return;
      uint64_t now = kk_archive_now();
      if (!final && generation - checkpointed < 16 && now && last_compile &&
          now - last_compile < 500000000) {
         [self scheduleLocked];
         return;
      }
   }
   /* Do not wait behind active captures. Independent native PSO compiles share
    * read access; only this short native snapshot needs exclusive access. */
   if (final) pthread_rwlock_wrlock(&capture_gate);
   else if (pthread_rwlock_trywrlock(&capture_gate)) {
      @synchronized(self) { [self scheduleLocked]; }
      return;
   }
   @synchronized(self) { target = generation; }
   NSString *path = kk_archive_temporary_path();
   NSError *error = nil;
   BOOL serialized = NO;
   uint64_t begin = kk_archive_now();
   @try {
      if (path) {
         [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
         serialized = [serializer serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:path] error:&error];
      }
   } @catch (NSException *exception) {
      serialized = NO;
   } @finally {
      pthread_rwlock_unlock(&capture_gate);
   }
   uint64_t duration = kk_archive_now() - begin;
   @synchronized(self) { stats.snapshot_ns += duration; }
   /* File reading, compression, ordered piece/index writes and verification
    * run after the capture gate is released, entirely on the utility queue. */
   NSData *data = serialized ? [NSData dataWithContentsOfFile:path] : nil;
   if (path) [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   if (data.length) {
      uint8_t identity[32];
      if (operations.store_piece(cache_data, data.bytes, data.length, identity)) {
         NSData *key = [NSData dataWithBytes:identity length:sizeof(identity)];
         KKArchivePiece *piece = [[KKArchivePiece alloc] initWithIdentity:key];
         if (piece) {
            NSMutableArray *index = [NSMutableArray array];
            @synchronized(self) {
               [pieces addObject:piece];
               while (pieces.count > 64) [pieces removeObjectAtIndex:0];
               for (KKArchivePiece *entry in pieces) [index addObject:entry->identity];
            }
            [piece release];
            NSData *manifest = [NSPropertyListSerialization dataWithPropertyList:index
               format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
            if (manifest.length) operations.store(cache_data, manifest.bytes, manifest.length);
            @synchronized(self) { stats.stored_bytes += data.length; stats.checkpoints++; }
         }
      }
   }
   @synchronized(self) {
      /* A failed flush/store is a cache miss, never a pipeline failure. A
       * flushed batch is not replayed on the compiler or retried in a loop. */
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
   [preferred release];
   [pieces release];
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
   [kk_compiler_archive(compiler) recordCompile];
}

static id<MTLComputePipelineState>
kk_archive_compute(mtl_compiler *compiler, MTL4ComputePipelineDescriptor *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return nil;
   for (KKArchivePiece *piece in [cache candidates]) {
      id<MTL4Archive> archive = [cache archiveForPiece:piece];
      if (!archive) continue;
      @synchronized(cache) { cache->stats.lookups++; }
      NSError *error = nil;
      id<MTLComputePipelineState> state = [archive newComputePipelineStateWithDescriptor:descriptor error:&error];
      if (state) { [cache recordHit:piece]; return state; }
   }
   return nil;
}

static id<MTLRenderPipelineState>
kk_archive_render(mtl_compiler *compiler, MTL4PipelineDescriptor *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return nil;
   for (KKArchivePiece *piece in [cache candidates]) {
      id<MTL4Archive> archive = [cache archiveForPiece:piece];
      if (!archive) continue;
      @synchronized(cache) { cache->stats.lookups++; }
      NSError *error = nil;
      id<MTLRenderPipelineState> state = [archive newRenderPipelineStateWithDescriptor:descriptor error:&error];
      if (state) { [cache recordHit:piece]; return state; }
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
