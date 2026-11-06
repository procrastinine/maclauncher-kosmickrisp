// SPDX-License-Identifier: MIT
// Keep command storage exclusive until the Vulkan recording can be discarded.
export const id = 'command-allocator-lifetime';

export function apply({edit}) {
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', source => source.replace(
`/* Ends Metal command buffer recording and returns allocator to pool for reuse */`,
`/* Ending recording preserves the allocator backing executable commands. */`).replace(
`   mtl_end_command_buffer(cmd->metal.cmd_buf);
   kk_cmd_pool_return_allocator(kk_cmd_buffer_pool(cmd), cmd->metal.allocator);
   cmd->metal.allocator = NULL;
   util_dynarray_foreach(&cmd->mesh_allocators, mtl_command_allocator *, allocator)
      kk_cmd_pool_return_allocator(kk_cmd_buffer_pool(cmd), *allocator);
   util_dynarray_clear(&cmd->mesh_allocators);
}

static void
kk_destroy_cmd_buffer`,
`   mtl_end_command_buffer(cmd->metal.cmd_buf);
}

static void
kk_release_command_allocators(struct kk_cmd_buffer *cmd)
{
   struct kk_cmd_pool *pool = kk_cmd_buffer_pool(cmd);

   /* Reset/destroy may discard the recording only after it is no longer
    * pending. Each allocator belongs exclusively to this Vulkan command
    * buffer, including every native part of a split mesh recording. */
   if (cmd->metal.allocator) {
      mtl_command_allocator_reset(cmd->metal.allocator);
      kk_cmd_pool_return_allocator(pool, cmd->metal.allocator);
      cmd->metal.allocator = NULL;
   }
   util_dynarray_foreach(&cmd->mesh_allocators, mtl_command_allocator *, allocator) {
      mtl_command_allocator_reset(*allocator);
      kk_cmd_pool_return_allocator(pool, *allocator);
   }
   util_dynarray_clear(&cmd->mesh_allocators);
}

static void
kk_destroy_cmd_buffer`).replace(
`   /* Ensure closed command buffer for safe returns to pool */
   end_recording(cmd);
   if (cmd->metal.cmd_buf)`,
`   /* Ensure closed command buffer for safe returns to pool */
   end_recording(cmd);
   kk_release_command_allocators(cmd);
   if (cmd->metal.cmd_buf)`).replace(
`   /* Ensure closed command buffer so we can start it again */
   end_recording(cmd);
   kk_cmd_release_resources(dev, cmd);`,
`   /* Ensure closed command buffer so we can start it again */
   end_recording(cmd);
   kk_release_command_allocators(cmd);
   kk_cmd_release_resources(dev, cmd);`).replace(
` * workgroups. Allocators return to the pool only after recording ends;
 * submission order and final query availability are preserved. */`,
` * workgroups. Allocators remain with the executable recording until reset
 * or destruction; submission order and query availability are preserved. */`));
  edit('src/kosmickrisp/vulkan/kk_cmd_pool.c', source => {
    const start = source.indexOf('static void\nkk_cmd_pool_release_mtl_objects(');
    const end = source.indexOf('\nVKAPI_ATTR void VKAPI_CALL\nkk_DestroyCommandPool(', start);
    if (start < 0 || end < 0) throw Error('Native pool release helper missing');
    const original = source.slice(start, end);
    const allocator_start = original.indexOf('   util_dynarray_foreach(&pool->metal.free_allocators,');
    const buffer_start = original.indexOf('   util_dynarray_foreach(&pool->metal.free_cmd_bufs,');
    const buffer_end = original.indexOf('   util_dynarray_clear(&pool->metal.free_cmd_bufs);') +
      '   util_dynarray_clear(&pool->metal.free_cmd_bufs);'.length;
    if (allocator_start < 0 || buffer_start <= allocator_start || buffer_end <= buffer_start)
      throw Error('Native pool release order changed');
    const ordered = original.slice(0, allocator_start) +
      '   /* Native buffer deallocation returns storage to its allocator. */\n' +
      original.slice(buffer_start, buffer_end) + '\n\n' +
      original.slice(allocator_start, buffer_start).trimEnd() +
      original.slice(buffer_end);
    return source.replace(original, ordered).replace(
      '   vk_command_pool_trim(&pool->vk, flags);\n   kk_cmd_pool_destroy_bos(pool);',
      '   vk_command_pool_trim(&pool->vk, flags);\n   kk_cmd_pool_release_mtl_objects(pool);\n   kk_cmd_pool_destroy_bos(pool);');
  });
  edit('src/kosmickrisp/bridge/mtl_command_buffer.m', source => source.replace(
    '#include <Metal/MTL4CommandBuffer.h>',
    '#include <Metal/MTL4CommandBuffer.h>\n#include <objc/runtime.h>\n\nstatic char kk_command_buffer_allocator_key;').replace(
    '      [cmd_buf beginCommandBufferWithAllocator:alloc];',
    '      /* The previous allocator must survive the native begin call that\n' +
    '       * returns its old-generation storage. Keep the new allocator alive\n' +
    '       * through native buffer deallocation, including after pool reset. */\n' +
    '      [cmd_buf beginCommandBufferWithAllocator:alloc];\n' +
    '      objc_setAssociatedObject(cmd_buf, &kk_command_buffer_allocator_key,\n' +
    '                               alloc, OBJC_ASSOCIATION_RETAIN_NONATOMIC);'));
  edit('src/kosmickrisp/vulkan/kk_cmd_buffer.c', source => source.replace(
    '   if (cmd->vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING)\n      return;',
    '   if (cmd->vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING ||\n       cmd->metal.cmd_buf == NULL)\n      return;').replace(
    '   /* Reset/destroy may discard the recording only after it is no longer',
    '   /* Native buffers must return their recorded storage before reset\n' +
    '    * makes the allocator heaps reusable. Do not cache recorded buffers\n' +
    '    * across that boundary with an outstanding native storage reference. */\n' +
    '   if (cmd->metal.cmd_buf) {\n' +
    '      mtl_release(cmd->metal.cmd_buf);\n' +
    '      cmd->metal.cmd_buf = NULL;\n' +
    '   }\n' +
    '   util_dynarray_foreach(&cmd->mesh_command_buffers, mtl_command_buffer *, part)\n' +
    '      mtl_release(*part);\n' +
    '   util_dynarray_clear(&cmd->mesh_command_buffers);\n\n' +
    '   /* Reset/destroy may discard the recording only after it is no longer').replace(
    '   kk_release_command_allocators(cmd);\n   if (cmd->metal.cmd_buf)\n      kk_cmd_pool_return_cmd_buf(pool, cmd->metal.cmd_buf);\n   cmd->metal.cmd_buf = NULL;',
    '   kk_release_command_allocators(cmd);').replace(
    '   util_dynarray_foreach(&cmd->mesh_command_buffers, mtl_command_buffer *, part)\n      kk_cmd_pool_return_cmd_buf(pool, *part);\n   util_dynarray_clear(&cmd->mesh_command_buffers);',
    '   /* Native parts retire with their allocator in kk_release_command_allocators. */').replace(
    '   vk_command_buffer_begin(&cmd->vk, pBeginInfo);\n   for (unsigned i = 0; i < 64; i++)',
    '   for (unsigned i = 0; i < 64; i++)').replace(
    '   /* If this is the first time starting the command buffer allocate Metal\n    * resources */',
    '   /* Implicit reset must discard old native storage before acquiring the\n' +
    '    * next recording, as explicit reset and destruction already do. */\n' +
    '   vk_command_buffer_begin(&cmd->vk, pBeginInfo);\n\n' +
    '   /* If this is the first time starting the command buffer allocate Metal\n    * resources */').replace(
    'fail_allocator:\n   kk_cmd_pool_return_cmd_buf(kk_cmd_buffer_pool(cmd), cmd->metal.cmd_buf);',
    'fail_allocator:\n   mtl_release(cmd->metal.cmd_buf);'));
}
