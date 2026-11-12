// SPDX-License-Identifier: MIT
// Emit shader-call scope memory barriers
export const id = "emit-shader-call-scope-memory-barriers";
export function apply({edit, edits}) {
  edit("src/kosmickrisp/compiler/nir_to_msl.c", source => source
    .replace("            break;\n         case SCOPE_QUEUE_FAMILY:\n         case SCOPE_DEVICE:\n            P(ctx, \"thread_scope::thread_scope_device\");\n            break;\n         default:\n",
      () => "            break;\n         case SCOPE_QUEUE_FAMILY:\n         case SCOPE_DEVICE:\n         /* Shader calls run as visible functions of the calling thread; the\n          * device scope also orders their image and buffer accesses. */\n         case SCOPE_SHADER_CALL:\n            P(ctx, \"thread_scope::thread_scope_device\");\n            break;\n         default:\n"));
}
