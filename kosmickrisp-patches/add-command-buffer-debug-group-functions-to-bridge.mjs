// SPDX-License-Identifier: MIT
// Add command buffer debug group functions to bridge
export const id = "add-command-buffer-debug-group-functions-to-bridge";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/bridge/mtl_command_buffer.h", source => source
    .replace("\nvoid mtl_command_buffer_set_label(mtl_command_buffer *cb, const char *label);\n\n#endif /* MTL_COMMAND_BUFFER_H */\n",
      () => "\nvoid mtl_command_buffer_set_label(mtl_command_buffer *cb, const char *label);\n\nvoid mtl_command_buffer_push_debug_group(mtl_command_buffer *command_buffer,\n                                         const char *label);\n\nvoid mtl_command_buffer_pop_debug_group(mtl_command_buffer *command_buffer);\n\n#endif /* MTL_COMMAND_BUFFER_H */\n"));
  edit("src/kosmickrisp/bridge/mtl_command_buffer.m", source => source
    .replace("      cb.label = @(label);\n   }\n}\n",
      () => "      cb.label = @(label);\n   }\n}\n\nvoid\nmtl_command_buffer_push_debug_group(mtl_command_buffer *command_buffer,\n                                    const char *label)\n{\n    @autoreleasepool {\n      id<MTL4CommandBuffer> cb = (id<MTL4CommandBuffer>)command_buffer;\n      [cb pushDebugGroup:@(label)];\n    }\n}\n\nvoid mtl_command_buffer_pop_debug_group(mtl_command_buffer *command_buffer)\n{\n   @autoreleasepool {\n      id<MTL4CommandBuffer> cb = (id<MTL4CommandBuffer>)command_buffer;\n      [cb popDebugGroup];\n   }\n}\n"));
  edit("src/kosmickrisp/bridge/stubs/mtl_command_buffer.c", source => source
    .replace("                             const char *label)\n{\n}\n",
      () => "                             const char *label)\n{\n}\n\nvoid\nmtl_command_buffer_push_debug_group(void *encoder, const char *label)\n{\n}\n\nvoid\nmtl_command_buffer_pop_debug_group(void *encoder)\n{\n}\n"));
}
