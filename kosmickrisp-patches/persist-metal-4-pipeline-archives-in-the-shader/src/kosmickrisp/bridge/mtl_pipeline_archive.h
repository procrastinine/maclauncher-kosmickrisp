/* SPDX-License-Identifier: MIT */
/* Private implementation; included only by mtl_compiler.m. */
#include <Metal/MTL4Archive.h>
#include <Metal/MTL4PipelineDataSetSerializer.h>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <unistd.h>

static char kk_archive_association;

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

@interface KKPipelineArchive : NSObject {
@public
   id<MTL4PipelineDataSetSerializer> serializer;
   id<MTL4Archive> archive;
   id<MTLDevice> device;
   struct mtl_compiler_cache_ops operations;
   void *cache_data;
   dispatch_once_t loaded;
   NSString *archive_path;
   NSMutableArray *hits;
   bool dirty, complete, finished;
   struct mtl_compiler_cache_stats stats;
}
- (id)initWithDevice:(id<MTLDevice>)dev operations:(const struct mtl_compiler_cache_ops *)ops data:(void *)data;
- (void)load;
- (void)recordHit:(MTL4PipelineDescriptor *)descriptor compute:(BOOL)compute;
- (void)recordCompile;
- (void)finish:(id<MTL4Compiler>)compiler;
@end

@implementation KKPipelineArchive
- (id)initWithDevice:(id<MTLDevice>)dev operations:(const struct mtl_compiler_cache_ops *)ops data:(void *)data
{
   self = [super init];
   if (!self) return nil;
   device = [dev retain];
   operations = *ops;
   cache_data = data;
   complete = true;
   hits = [[NSMutableArray alloc] init];
   MTL4PipelineDataSetSerializerDescriptor *desc = [[MTL4PipelineDataSetSerializerDescriptor new] autorelease];
   desc.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
   serializer = [dev newPipelineDataSetSerializerWithDescriptor:desc];
   return self;
}

- (void)load
{
   dispatch_once(&loaded, ^{
      size_t length = 0;
      void *bytes = operations.load(cache_data, &length);
      if (!bytes) return;
      NSData *data = [NSData dataWithBytesNoCopy:bytes length:length freeWhenDone:YES];
      NSString *path = kk_archive_temporary_path();
      NSError *error = nil;
      if (path && [data writeToFile:path options:NSDataWritingAtomic error:&error]) {
         archive = [device newArchiveWithURL:[NSURL fileURLWithPath:path] error:&error];
         if (archive) archive_path = [path copy];
      }
      if (!archive && path)
         [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   });
}

- (void)recordHit:(MTL4PipelineDescriptor *)descriptor compute:(BOOL)compute
{
   id copy = serializer ? [descriptor copy] : nil;
   @synchronized(self) {
      stats.hits++;
      if (serializer && copy && hits)
         [hits addObject:@{@"descriptor":copy, @"compute":@(compute)}];
      else if (serializer)
         complete = false;
   }
   [copy release];
}

- (void)recordCompile
{
   @synchronized(self) { dirty = true; stats.native_compiles++; }
}

/* Final compiler client only: no renderer or compile task can still use it.
 * Replay cache hits here so the new capture contains the whole used set,
 * instead of replacing the old archive with only this launch's misses. */
- (void)finish:(id<MTL4Compiler>)compiler
{
   if (finished) return;
   finished = true;
   if (!serializer || !dirty || !complete) {
      [hits removeAllObjects];
      return;
   }
   bool replayed = true;
   for (NSDictionary *hit in hits) {
      NSError *error = nil;
      id state = [hit[@"compute"] boolValue]
         ? (id)[compiler newComputePipelineStateWithDescriptor:hit[@"descriptor"] compilerTaskOptions:nil error:&error]
         : (id)[compiler newRenderPipelineStateWithDescriptor:hit[@"descriptor"] compilerTaskOptions:nil error:&error];
      if (!state) { replayed = false; break; }
      [state release];
   }
   [hits removeAllObjects];
   if (!replayed) return; /* Preserve the previous valid archive. */
   NSString *path = kk_archive_temporary_path();
   if (!path) return;
   [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
   NSError *error = nil;
   if ([serializer serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:path] error:&error]) {
      NSData *data = [NSData dataWithContentsOfFile:path];
      if (data.length) {
         operations.store(cache_data, data.bytes, data.length);
         stats.stored_bytes = data.length;
      }
   }
   [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
}

- (void)dealloc
{
   [archive release];
   [serializer release];
   [device release];
   [hits release];
   if (archive_path) {
      [[NSFileManager defaultManager] removeItemAtPath:archive_path error:NULL];
      [archive_path release];
   }
   operations.destroy(cache_data);
   [super dealloc];
}
@end

static KKPipelineArchive *
kk_compiler_archive(mtl_compiler *compiler)
{
   return objc_getAssociatedObject((id)compiler, &kk_archive_association);
}

static id<MTLComputePipelineState>
kk_archive_compute(mtl_compiler *compiler, MTL4ComputePipelineDescriptor *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return nil;
   [cache load];
   if (!cache->archive) return nil;
   @synchronized(cache) { cache->stats.lookups++; }
   NSError *error = nil;
   id<MTLComputePipelineState> state = [cache->archive newComputePipelineStateWithDescriptor:descriptor error:&error];
   if (state) [cache recordHit:descriptor compute:YES];
   return state;
}

static id<MTLRenderPipelineState>
kk_archive_render(mtl_compiler *compiler, MTL4PipelineDescriptor *descriptor)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return nil;
   [cache load];
   if (!cache->archive) return nil;
   @synchronized(cache) { cache->stats.lookups++; }
   NSError *error = nil;
   id<MTLRenderPipelineState> state = [cache->archive newRenderPipelineStateWithDescriptor:descriptor error:&error];
   if (state) [cache recordHit:descriptor compute:NO];
   return state;
}

bool
mtl_compiler_get_cache_stats(mtl_compiler *compiler,
                            struct mtl_compiler_cache_stats *stats)
{
   KKPipelineArchive *cache = kk_compiler_archive(compiler);
   if (!cache) return false;
   @synchronized(cache) {
      *stats = cache->stats;
      stats->archive_loaded = cache->archive != nil;
   }
   return true;
}
