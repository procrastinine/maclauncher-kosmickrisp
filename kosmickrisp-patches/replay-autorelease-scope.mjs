// SPDX-License-Identifier: MIT
import fs from 'node:fs';
export const id = 'replay-autorelease-scope';
export const sourceFiles = ['kosmickrisp-patches/kosmickrisp-replay-autorelease.h'];
export function apply({edit, edits}) {
  edits.set('src/kosmickrisp/bridge/mtl_replay_autorelease.h',
    fs.readFileSync(new URL('./kosmickrisp-replay-autorelease.h', import.meta.url), 'utf8'));
  edit('src/kosmickrisp/bridge/mtl_bridge.h', source => source.replace(
    '#include "mtl_types.h"', '#include "mtl_types.h"\n#include "mtl_replay_autorelease.h"'));
  edit('src/kosmickrisp/bridge/mtl_bridge.m', source => source.replace(
    '#include "mtl_bridge.h"', '#include "mtl_bridge.h"\n#include "vk_cmd_queue.h"\n#import <Foundation/NSAutoreleasePool.h>') + "\n/* Thread-local dynamic extent, never shared between recording workers. */\nstatic _Thread_local bool kk_mtl_replay_pool_active;\n\nbool\nmtl_replay_autorelease_pool_active(void)\n{\n   return kk_mtl_replay_pool_active;\n}\n\nvoid\nmtl_execute_command_queue(struct vk_cmd_queue *queue,\n                          VkCommandBuffer command_buffer,\n                          const struct vk_device_dispatch_table *dispatch)\n{\n   NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];\n   if (pool == nil) {\n      /* An existing outer scope or the standalone wrappers still owns pools. */\n      vk_cmd_queue_execute(queue, command_buffer, dispatch);\n      return;\n   }\n   const bool previous = kk_mtl_replay_pool_active;\n   kk_mtl_replay_pool_active = true;\n   @try {\n      vk_cmd_queue_execute(queue, command_buffer, dispatch);\n   } @finally {\n      /* Restore before draining: deallocation may reenter the bridge. */\n      kk_mtl_replay_pool_active = previous;\n      [pool drain];\n   }\n}\n");
  edit('src/kosmickrisp/bridge/stubs/mtl_bridge.c', source => source.replace(
    '#include "mtl_bridge.h"', '#include "mtl_bridge.h"\n#include "vk_cmd_queue.h"') + "\nbool\nmtl_replay_autorelease_pool_active(void)\n{\n   return false;\n}\n\nvoid\nmtl_execute_command_queue(struct vk_cmd_queue *queue,\n                          VkCommandBuffer command_buffer,\n                          const struct vk_device_dispatch_table *dispatch)\n{\n   vk_cmd_queue_execute(queue, command_buffer, dispatch);\n}\n");
  for (const name of ['mtl_encoder', 'mtl_render_state', 'mtl_argument_table']) {
    edit('src/kosmickrisp/bridge/' + name + '.m', source => source.replace(
      '#include "' + name + '.h"',
      '#include "' + name + '.h"\n#include "mtl_replay_autorelease.h"').replace(
      /^([ \t]+)@autoreleasepool \{([\s\S]*?)\n\1\}\n\}/gm,
      (match, indent, body) => indent + 'KK_MTL_REPLAY_POOL({' + body + '\n' + indent + '});\n}'));
  }
  for (const name of ['kk_cmd_buffer', 'kk_queue']) {
    edit('src/kosmickrisp/vulkan/' + name + '.c', source => source.replaceAll(
      'vk_cmd_queue_execute(', 'mtl_execute_command_queue('));
  }
}
