// SPDX-License-Identifier: MIT
// Share replay pool for native retain and release
export const id = "share-replay-pool-for-native-retain-and-release";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_bridge.m", source => source
    .replace("void *\nmtl_retain(void *handle)\n{\n   @autoreleasepool {\n      NSObject *obj = (NSObject *)handle;\n      return [obj retain];\n   }\n}\n\nvoid\nmtl_release(void *handle)\n{\n   @autoreleasepool {\n      NSObject *obj = (NSObject *)handle;\n      [obj release];\n   }\n}\n\n/* Thread-local dynamic extent, never shared between recording workers. */\n",
      () => "void *\nmtl_retain(void *handle)\n{\n   KK_MTL_REPLAY_POOL({\n      NSObject *obj = (NSObject *)handle;\n      return [obj retain];\n   });\n}\n\nvoid\nmtl_release(void *handle)\n{\n   KK_MTL_REPLAY_POOL({\n      NSObject *obj = (NSObject *)handle;\n      [obj release];\n   });\n}\n\n/* Thread-local dynamic extent, never shared between recording workers. */\n"));
}
