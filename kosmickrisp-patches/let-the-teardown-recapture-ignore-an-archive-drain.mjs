// SPDX-License-Identifier: MIT
// Let the teardown recapture ignore an archive drain
export const id = "let-the-teardown-recapture-ignore-an-archive-drain";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_pipeline_archive.h", source => source
    .replace("   if (!cache || !__atomic_load_n(&cache->recapture_count, __ATOMIC_ACQUIRE)) return;\n   for (unsigned i = 0; i < limit; ++i) @autoreleasepool {\n      NSArray *entry = nil;\n      if (__atomic_load_n(&cache->draining, __ATOMIC_ACQUIRE)) break;\n      @synchronized(cache) {\n         if (cache->finished || !cache->recapture.count) break;\n         entry = [[cache->recapture objectAtIndex:0] retain];\n",
      () => "   if (!cache || !__atomic_load_n(&cache->recapture_count, __ATOMIC_ACQUIRE)) return;\n   for (unsigned i = 0; i < limit; ++i) @autoreleasepool {\n      NSArray *entry = nil;\n      /* Per-compile batches yield to a drain; the unbounded call made at\n       * teardown, after the last client, captures everything left. */\n      if (limit != UINT_MAX && __atomic_load_n(&cache->draining, __ATOMIC_ACQUIRE)) break;\n      @synchronized(cache) {\n         if (cache->finished || !cache->recapture.count) break;\n         entry = [[cache->recapture objectAtIndex:0] retain];\n"));
}
