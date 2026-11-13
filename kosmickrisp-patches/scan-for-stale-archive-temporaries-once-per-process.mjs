// SPDX-License-Identifier: MIT
// Scan for stale archive temporaries once per process
export const id = "scan-for-stale-archive-temporaries-once-per-process";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_pipeline_archive.h", source => source
    .replace("      available = operations.load_piece && operations.store_piece &&\n         (!operations.available || operations.available(cache_data));\n      if (!available) return;\n      dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{\n         @autoreleasepool { kk_archive_remove_stale_temporaries(); }\n      });\n      size_t length = 0;\n      void *bytes = operations.load(cache_data, &length);\n",
      () => "      available = operations.load_piece && operations.store_piece &&\n         (!operations.available || operations.available(cache_data));\n      if (!available) return;\n      /* One scan per process. Each device used to queue its own scan of the\n       * whole temporary directory; with many devices those background scans\n       * occupied the background workers that deferred compiles and their\n       * operation completion depend on, and a waiting draw stalled. */\n      static dispatch_once_t stale_scan;\n      dispatch_once(&stale_scan, ^{\n         dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{\n            @autoreleasepool { kk_archive_remove_stale_temporaries(); }\n         });\n      });\n      size_t length = 0;\n      void *bytes = operations.load(cache_data, &length);\n"));
}
